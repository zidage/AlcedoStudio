//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_film_print_pass.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/graph/film_print_model.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

// film_print.metal has the same values, and a different size stops the build of its library.
static_assert(kFilmBlurMaxRadius == 128 && sizeof(FilmBlurKernel) == 1552);
/// FILM_SLIDE of film_print.metal: the outputs of one thread of a blur pass.
constexpr std::uint32_t kSlide = 8;
static_assert(kFilmNarrowSpreadMaxRadius == 2 && sizeof(FilmNarrowKernel) == 76);
static_assert(sizeof(FilmKernelParams) == 416);
static_assert(sizeof(Matrix3x3) == 36);
static_assert(sizeof(FilmKernelRgb) == 12);

/// `FilmSize` of film_print.metal.
struct Size {
  std::int32_t width  = 0;
  std::int32_t height = 0;
};

auto MakeSize(std::uint32_t width, std::uint32_t height) -> Size {
  return {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

auto MakeSize(ImageExtent extent) -> Size { return MakeSize(extent.width, extent.height); }

/// `FilmDisplayArguments` of film_print.metal.
struct DisplayArguments {
  Matrix3x3    render_to_flare{};
  float        flare_strength = 0.0f;
  std::int32_t has_flare      = 0;
  std::int32_t has_soft       = 0;
  std::int32_t soft_half_size = 0;
  Size         soft_cells{};
};
static_assert(sizeof(DisplayArguments) == 60);

/// `FilmBlurWeights` of film_print.metal: one tap of the three layers in one float4. The last
/// chunk of a blur loop reads up to kSlide - 1 taps past the radius, which hold 0.
struct BlurWeights {
  std::uint32_t radius = 0;
  std::uint32_t unused[3]{};
  float         w[kFilmBlurMaxRadius + kSlide][4]{};
};
static_assert(sizeof(BlurWeights) == 2192);

auto MakeWeights(const FilmBlurKernel& kernel) -> BlurWeights {
  BlurWeights weights;
  weights.radius = kernel.radius;
  for (std::uint32_t tap = 0; tap <= kernel.radius; ++tap) {
    for (int layer = 0; layer < 3; ++layer) weights.w[tap][layer] = kernel.weights[layer][tap];
  }
  return weights;
}

/// The texel forms of film_print.metal. See its header comment for the images of each form.
enum class PlaneForm : std::uint32_t {
  Float = 12,  ///< packed_float3
  Half  = 8,   ///< half4
};

/// One image between the steps, in the transient arena of the render.
struct Plane {
  MTL::Buffer*  native = nullptr;
  std::uint32_t offset = 0;  ///< In bytes from the start of @ref native
};

auto Native(const MetalBackend::Texture2D& texture) -> MTL::Texture* {
  return static_cast<MTL::Texture*>(texture.Native());
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

/**
 * @brief One dispatch on the serial compute encoder of the frame.
 *
 * The constructor binds the pipeline state of @p function from the pipeline cache. Each setter
 * binds one argument at the index that film_print.metal states. @ref Run dispatches and orders
 * later dispatches after its writes.
 */
class Dispatch {
 public:
  Dispatch(MetalRenderDevice& device, const char* function) : device_(device) {
#ifndef ALCEDO_METAL_DRT_METALLIB_PATH
    (void)function;
    throw std::runtime_error("RecordMetalFilmPrint: the Metal DRT metallib path is not configured");
#else
    pipeline_ = metal::ComputePipelineCache::Instance().GetPipelineState(
        ALCEDO_METAL_DRT_METALLIB_PATH, function, "Metal Film Print");
    if (!pipeline_) {
      throw std::runtime_error(std::string{"RecordMetalFilmPrint: no pipeline state for "} +
                               function);
    }
    encoder_ = static_cast<MTL::ComputeCommandEncoder*>(
        device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
    if (encoder_ == nullptr) {
      throw std::runtime_error("RecordMetalFilmPrint: compute encoder is missing");
    }
    encoder_->setComputePipelineState(pipeline_.get());
#endif
  }

  auto Buffer(NS::UInteger index, const Plane& plane) -> Dispatch& {
    encoder_->setBuffer(plane.native, plane.offset, index);
    return *this;
  }

  auto Buffer(NS::UInteger index, void* native) -> Dispatch& {
    encoder_->setBuffer(static_cast<MTL::Buffer*>(native), 0, index);
    return *this;
  }

  template <class Value>
  auto Bytes(NS::UInteger index, const Value& value) -> Dispatch& {
    static_assert(sizeof(Value) <= 4096, "setBytes allows at most 4 KB");
    encoder_->setBytes(&value, sizeof(Value), index);
    return *this;
  }

  auto Texture(NS::UInteger index, MTL::Texture* texture) -> Dispatch& {
    encoder_->setTexture(texture, index);
    return *this;
  }

  /// One thread for each pixel of @p width x @p height.
  void Run(std::uint32_t width, std::uint32_t height) {
    const auto thread_width = std::max<NS::UInteger>(1, pipeline_->threadExecutionWidth());
    const auto thread_height =
        std::max<NS::UInteger>(1, pipeline_->maxTotalThreadsPerThreadgroup() / thread_width);
    Encode(width, height, thread_width, thread_height);
  }

  /// A vertical blur pass: one thread for kSlide rows of a column.
  void RunDown(std::uint32_t width, std::uint32_t height) {
    RunBlur(width, (height + kSlide - 1) / kSlide);
  }

  /// A horizontal blur pass: one thread for kSlide columns of a row.
  void RunAcross(std::uint32_t width, std::uint32_t height) {
    RunBlur((width + kSlide - 1) / kSlide, height);
  }

 private:
  /// Blur passes hold a window in registers, so they use groups of 8 SIMD rows.
  void RunBlur(std::uint32_t width, std::uint32_t height) {
    const auto thread_width = std::max<NS::UInteger>(1, pipeline_->threadExecutionWidth());
    const auto thread_height =
        std::clamp<NS::UInteger>(pipeline_->maxTotalThreadsPerThreadgroup() / thread_width, 1, 8);
    Encode(width, height, thread_width, thread_height);
  }

  void Encode(std::uint32_t width, std::uint32_t height, NS::UInteger thread_width,
              NS::UInteger thread_height) {
    encoder_->dispatchThreads(MTL::Size{width, height, 1},
                              MTL::Size{thread_width, thread_height, 1});
    device_.Workspace().Device().NoteComputeDispatch(device_.CommandContext());
  }

  MetalRenderDevice&                       device_;
  NS::SharedPtr<MTL::ComputePipelineState> pipeline_;
  MTL::ComputeCommandEncoder*              encoder_ = nullptr;
};

auto PlaneBytes(std::uint32_t width, std::uint32_t height, PlaneForm form) -> std::size_t {
  return static_cast<std::size_t>(width) * height * static_cast<std::uint32_t>(form);
}

auto AllocatePlane(MetalRenderDevice& device, std::uint32_t width, std::uint32_t height,
                   PlaneForm form) -> Plane {
  auto&      workspace = device.Workspace();
  const auto bytes     = PlaneBytes(width, height, form);
  void*      memory    = workspace.TransientBuffers().Allocate(bytes);
  if (memory == nullptr) {
    throw std::runtime_error("RecordMetalFilmPrint: transient allocation failed");
  }
  const auto resolved = workspace.Device().ResolveDeviceMemory(memory, bytes);
  return Plane{static_cast<MTL::Buffer*>(resolved.first), resolved.second};
}

/// The context of one recording.
struct Recording {
  MetalRenderDevice& device;
};

auto FormName(PlaneForm form) -> const char* { return form == PlaneForm::Float ? "float" : "half"; }

/// The name of a blur kernel from @p source to @p destination.
auto BlurName(const char* function, PlaneForm source, PlaneForm destination) -> std::string {
  return std::string{function} + "_" + FormName(source) + "_" + FormName(destination);
}

void BlurHorizontal(const Recording& r, PlaneForm source, PlaneForm destination, const Plane& src,
                    const Plane& dst, Size size, const FilmBlurKernel& taps) {
  Dispatch(r.device, BlurName("film_print_blur_horizontal", source, destination).c_str())
      .Buffer(0, src)
      .Buffer(1, dst)
      .Bytes(2, size)
      .Bytes(3, MakeWeights(taps))
      .RunAcross(size.width, size.height);
}

void BlurVertical(const Recording& r, PlaneForm form, const Plane& src, const Plane& dst, Size size,
                  const FilmBlurKernel& taps, float weight, bool accumulate) {
  const std::int32_t add = accumulate ? 1 : 0;
  Dispatch(r.device, BlurName("film_print_blur_vertical", form, form).c_str())
      .Buffer(0, src)
      .Buffer(1, dst)
      .Bytes(2, size)
      .Bytes(3, MakeWeights(taps))
      .Bytes(4, weight)
      .Bytes(5, add)
      .RunDown(size.width, size.height);
}

/// The last pass of a canonical result, from a half image.
void BlurVerticalToTexture(const Recording& r, const Plane& src, MTL::Texture* dst, Size size,
                           const FilmBlurKernel& taps) {
  Dispatch(r.device, "film_print_blur_vertical_to_texture")
      .Buffer(0, src)
      .Bytes(1, size)
      .Bytes(2, MakeWeights(taps))
      .Texture(0, dst)
      .RunDown(size.width, size.height);
}

/// What a canvas holds. The same values as CanvasValue of the CUDA and OpenCL passes.
enum class CanvasValue {
  kValue,          ///< The exposure, for Halation
  kTransmittance,  ///< 10^-a of the amounts, for the Enlarger flare of a print
  kDisplayLight,   ///< Linear display light of the light table, for the loupe flare
};

/// @p print_rgba and @p params are read only for CanvasValue::kDisplayLight.
void CanvasReduce(const Recording& r, const Plane& src, Size size, const Plane& dst,
                  ImageExtent extent, const Matrix3x3& canvas_to_render, std::uint32_t samples,
                  CanvasValue value, void* print_rgba, const FilmKernelParams& params) {
  const auto  samples_value = static_cast<std::int32_t>(samples);
  const char* function =
      value == CanvasValue::kTransmittance  ? "film_print_canvas_reduce_transmittance"
      : value == CanvasValue::kDisplayLight ? "film_print_canvas_reduce_display_light"
                                            : "film_print_canvas_reduce_exposure";
  Dispatch dispatch(r.device, function);
  dispatch.Buffer(0, src)
      .Buffer(1, dst)
      .Bytes(2, size)
      .Bytes(3, MakeSize(extent))
      .Bytes(4, canvas_to_render)
      .Bytes(5, samples_value);
  if (value == CanvasValue::kDisplayLight) dispatch.Buffer(6, print_rgba).Bytes(7, params);
  dispatch.Run(extent.width, extent.height);
}

using Lease = ResourceLease<MetalBackend>;

/**
 * @brief The canonical result @p id: the published texture, or a texture from @p build.
 *
 * The rule of `DecideDiffusionScatter`: a render that covers the full edit space builds and
 * publishes; an ROI samples a published texture, and otherwise builds a temporary texture from its
 * own pixels. Nothing is visible to another render before `PublishResults`.
 */
template <class Build>
auto ReadOrBuildCanvasResult(MetalRenderDevice& device, const ExecutionPlan& plan,
                             const GraphValueId& id, ImageExtent extent, std::uint32_t long_edge,
                             std::optional<Lease>& temporary, Build&& build) -> MTL::Texture* {
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
    return Native(canonical->Texture());
  }
  MTL::Texture* target = nullptr;
  if (decision.persist_canonical) {
    target = Native(
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::Rgba32f})
            .Texture());
  } else {
    temporary.emplace(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    target = Native(temporary->Texture());
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

void RecordMetalFilmPrint(MetalRenderDevice& device, const ExecutionPlan& plan,
                          const DrtNodeModel& drt, std::optional<double> raw_cct,
                          const FrameSceneBinding& scene, const FrameSceneBinding& display,
                          std::uint32_t width, std::uint32_t height,
                          std::vector<std::string>* pass_names, bool separate_narrow_spread) {
  const auto resolved =
      DrtOutputResolver::ResolveNode(drt, plan.output_color_override, "ExecuteMetalDrt");
  const auto  film        = drt.Params().FilmPrint();
  // Print material Negative Scan has the model layer of issue 661; its Metal pass is issue 662.
  // There is no other form to substitute for it.
  if (IsFilmNegativeScan(film.negative, film.print_material)) {
    throw std::runtime_error(
        "RecordMetalFilmPrint: print material Negative Scan has no Metal pass yet (issue 662)");
  }
  auto&       tables      = SharedFilmPrintTables();
  const auto& bundle      = tables.Bundle();
  const auto  light       = bundle.SceneLightMix(film.scene_light, raw_cct);
  const auto  tables_data = tables.Resolve(film.negative, film.print_material, light);

  auto&       workspace   = device.Workspace();
  auto&       backend     = workspace.Device();
  auto&       context     = device.CommandContext();
  const auto  print       = backend.AcquireLut(tables_data->print_key, tables_data->print_rgba,
                                               tables_data->edge, context);
  const auto  table       = backend.AcquireKeyedBuffer(
      tables_data->kernel_key, std::as_bytes(std::span(tables_data->kernel_values)), context);

  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(bundle, film.negative, film.print_material, light,
                                       tables_data->edge, params);
  params.display_encoding = static_cast<std::uint32_t>(resolved.eotf_);

  const auto negative     = bundle.Film(film.negative);
  const auto image        = MakeFilmPrintImagePlan(
      plan.geometry, bundle.ImagePlaneConstants(), negative.layer_spread_um,
      params.inhibition_has_image != 0U || params.negative_grade != 0U, film);
  // With no blur, one dispatch computes the final amounts, so FP4 and FP5 need no other image.
  const bool inhibitor_blur = image.image_plane && params.inhibition_has_image != 0U;

  // The previous render has completed (BeginRender), so an idle slab has no queued work.
  workspace.TransientBuffers().ReleaseIdleSlabsWhenNoneHolds(
      PlaneBytes(width, height, PlaneForm::Float));
  const Recording     recording{device};
  const Size          size        = MakeSize(width, height);
  void*               values      = table.native;
  void*               print_rgba  = print.native;
  auto&               scene_image = MetalSceneTexture(device, scene);
  // A texture argument that a dispatch does not read gets the scene, which it may read.
  MTL::Texture*       unused      = Native(scene_image);
  PassNames           names(pass_names);

  // FP1. The alpha mark: see film_print_display.
  Plane               q          = AllocatePlane(device, width, height, PlaneForm::Float);
  const Plane         alpha_mark = AllocatePlane(device, 1, 1, PlaneForm::Float);
  const std::uint32_t stamp = static_cast<std::uint32_t>(device.CommandContext().SubmissionId());
  Dispatch(device, "film_print_exposure")
      .Texture(0, Native(scene_image))
      .Buffer(0, q)
      .Buffer(1, values)
      .Bytes(2, params)
      .Bytes(3, size)
      .Buffer(4, alpha_mark)
      .Bytes(5, stamp)
      .Run(width, height);
  names.Mark("FP1 exposure");

  // FP2, rule 'spread_per_layer_pass'. After the spread, the first image holds the amounts.
  std::optional<Plane> scratch;
  Plane                a{};
  if (image.spread_passes != 0U) {
    const Plane spread = AllocatePlane(device, width, height, PlaneForm::Float);
    // The middle image of FP2. The inhibitor blur at full size uses it again for its middle image.
    scratch.emplace(AllocatePlane(device, width, height, PlaneForm::Float));
    if (image.spread_narrow_fused && !separate_narrow_spread) {
      BlurHorizontal(recording, PlaneForm::Float, PlaneForm::Float, q, *scratch, size,
                     image.spread_x[1]);
      names.Mark("FP2 wide horizontal");
      Dispatch(device, "film_print_spread_vertical")
          .Buffer(0, *scratch)
          .Buffer(1, q)
          .Buffer(2, spread)
          .Bytes(3, size)
          .Bytes(4, MakeWeights(image.spread_y[1]))
          .Bytes(5, image.spread_weight[1])
          .Bytes(6, image.spread_narrow)
          .Bytes(7, image.spread_weight[0])
          .RunDown(width, height);
      names.Mark("FP2 vertical, with the narrow Gaussian");
    } else {
      for (std::uint32_t pass = 0; pass < image.spread_passes; ++pass) {
        BlurHorizontal(recording, PlaneForm::Float, PlaneForm::Float, q, *scratch, size,
                       image.spread_x[pass]);
        BlurVertical(recording, PlaneForm::Float, *scratch, spread, size, image.spread_y[pass],
                     image.spread_weight[pass], pass != 0U);
      }
      names.Mark("FP2 spread, 2 separate passes");
    }
    a = q;
    q = spread;
  } else {
    a = AllocatePlane(device, width, height, PlaneForm::Float);
  }

  const auto           long_edge   = (std::max)(width, height);
  const auto           canvas      = image.canvas_extent;
  const Size           canvas_size = MakeSize(canvas);

  // FP3: the canonical texture, then a read of it per layer, in one dispatch with the first
  // amounts.
  std::optional<Lease> temporary_halation;
  MTL::Texture*        halation = unused;
  if (image.halation_active) {
    halation = ReadOrBuildCanvasResult(
        device, plan, FilmHalationResultId(plan.drt.node_id), canvas, long_edge, temporary_halation,
        [&](MTL::Texture* target) {
          const Plane reduced = AllocatePlane(device, canvas.width, canvas.height, PlaneForm::Half);
          const Plane middle  = AllocatePlane(device, canvas.width, canvas.height, PlaneForm::Half);
          CanvasReduce(recording, q, size, reduced, canvas, image.canvas_to_render,
                       image.canvas_samples, CanvasValue::kValue, print_rgba, params);
          BlurHorizontal(recording, PlaneForm::Half, PlaneForm::Half, reduced, middle, canvas_size,
                         image.halation_x);
          BlurVerticalToTexture(recording, middle, target, canvas_size, image.halation_y);
        });
    names.Mark("FP3 canvas");
  }
  const FilmKernelRgb halation_strength{image.halation_strength[0], image.halation_strength[1],
                                        image.halation_strength[2]};
  const std::int32_t  inhibitor_blur_flag = inhibitor_blur ? 1 : 0;

  // FP4 and FP5, rules 'inhibition_two_steps' and 'inhibition_image_part'. After FP4, the image of
  // the exposure holds the log exposure that the steps read.
  if (inhibitor_blur && image.inhibitor_half_size) {
    // Only the last step writes full-size amounts. The first amounts and the first step write the
    // box mean directly.
    const auto  extent  = image.inhibitor_extent;
    const Size  cells   = MakeSize(extent);
    const Plane reduced = AllocatePlane(device, extent.width, extent.height, PlaneForm::Half);
    const Plane middle  = AllocatePlane(device, extent.width, extent.height, PlaneForm::Half);
    const std::int32_t has_canvas = image.halation_active ? 1 : 0;
    Dispatch(device, "film_print_reduced_first_amounts")
        .Buffer(0, q)
        .Buffer(1, reduced)
        .Buffer(2, values)
        .Bytes(3, params)
        .Bytes(4, size)
        .Bytes(5, cells)
        .Bytes(6, has_canvas)
        .Bytes(7, image.render_to_canvas)
        .Bytes(8, halation_strength)
        .Texture(0, halation)
        .Run(extent.width, extent.height);
    names.Mark("FP3 mix and FP4 first amounts, to half size");
    BlurHorizontal(recording, PlaneForm::Half, PlaneForm::Half, reduced, middle, cells,
                   image.inhibitor_x);
    BlurVertical(recording, PlaneForm::Half, middle, reduced, cells, image.inhibitor_y, 1.0f,
                 false);
    names.Mark("FP4 inhibitor blur");
    Dispatch(device, "film_print_step_to_reduced")
        .Buffer(0, q)
        .Buffer(1, reduced)
        .Buffer(2, middle)
        .Buffer(3, values)
        .Bytes(4, params)
        .Bytes(5, size)
        .Bytes(6, cells)
        .Run(extent.width, extent.height);
    names.Mark("FP5 first step, to half size");
    BlurHorizontal(recording, PlaneForm::Half, PlaneForm::Half, middle, reduced, cells,
                   image.inhibitor_x);
    BlurVertical(recording, PlaneForm::Half, reduced, middle, cells, image.inhibitor_y, 1.0f,
                 false);
    names.Mark("FP4 inhibitor blur");
    Dispatch(device, "film_print_step_half_size")
        .Buffer(0, q)
        .Buffer(1, a)
        .Buffer(2, middle)
        .Buffer(3, values)
        .Bytes(4, params)
        .Bytes(5, size)
        .Bytes(6, cells)
        .Run(width, height);
    names.Mark("FP5 last step");
  } else {
    // With the blur, the amounts before the last step are only the input of the next blur, so they
    // are half. The last step writes the float amounts of the later stages.
    const PlaneForm first_form = inhibitor_blur ? PlaneForm::Half : PlaneForm::Float;
    const Plane first = inhibitor_blur ? AllocatePlane(device, width, height, PlaneForm::Half) : a;
    const std::string form_suffix = std::string{"_"} + FormName(first_form);
    if (image.halation_active) {
      Dispatch(device, ("film_print_halation_first_amounts" + form_suffix).c_str())
          .Buffer(0, q)
          .Buffer(1, first)
          .Buffer(2, values)
          .Bytes(3, params)
          .Bytes(4, size)
          .Bytes(5, inhibitor_blur_flag)
          .Bytes(6, image.render_to_canvas)
          .Bytes(7, halation_strength)
          .Texture(0, halation)
          .Run(width, height);
      names.Mark("FP3 mix and FP4 amounts");
    } else {
      Dispatch(device, ("film_print_first_amounts" + form_suffix).c_str())
          .Buffer(0, q)
          .Buffer(1, first)
          .Buffer(2, values)
          .Bytes(3, params)
          .Bytes(4, size)
          .Bytes(5, inhibitor_blur_flag)
          .Run(width, height);
      names.Mark("FP4 amounts");
    }
    if (inhibitor_blur) {
      if (!scratch.has_value()) {
        scratch.emplace(AllocatePlane(device, width, height, PlaneForm::Half));
      }
      for (int step = 0; step < 2; ++step) {
        const bool last = step == 1;
        BlurHorizontal(recording, PlaneForm::Half, PlaneForm::Half, first, *scratch, size,
                       image.inhibitor_x);
        names.Mark("FP4 inhibitor horizontal");
        Dispatch(device, last ? "film_print_step_vertical_float" : "film_print_step_vertical_half")
            .Buffer(0, *scratch)
            .Buffer(1, q)
            .Buffer(2, last ? a : first)
            .Buffer(3, values)
            .Bytes(4, params)
            .Bytes(5, size)
            .Bytes(6, MakeWeights(image.inhibitor_y))
            .RunDown(width, height);
        names.Mark("FP4 vertical and FP5 inhibited step");
      }
    }
  }

  // Rule 'negative_show_grade': the blurred amounts, before the flare of FP6.
  std::optional<Plane> soft;
  if (image.image_plane && params.negative_grade != 0U) {
    const auto extent      = image.inhibitor_extent;
    const Size cells       = MakeSize(extent);
    Plane      source      = a;
    PlaneForm  source_form = PlaneForm::Float;
    if (image.inhibitor_half_size) {
      const Plane reduced = AllocatePlane(device, extent.width, extent.height, PlaneForm::Half);
      Dispatch(device, "film_print_amounts_reduce")
          .Buffer(0, a)
          .Buffer(1, reduced)
          .Bytes(2, size)
          .Bytes(3, cells)
          .Run(extent.width, extent.height);
      source      = reduced;
      source_form = PlaneForm::Half;
    }
    const Plane middle = AllocatePlane(device, extent.width, extent.height, PlaneForm::Half);
    soft.emplace(AllocatePlane(device, extent.width, extent.height, PlaneForm::Half));
    BlurHorizontal(recording, source_form, PlaneForm::Half, source, middle, cells,
                   image.inhibitor_x);
    BlurVertical(recording, PlaneForm::Half, middle, *soft, cells, image.inhibitor_y, 1.0f, false);
    names.Mark("FP6 blurred amounts of the negative grade");
  }

  // FP6: the canonical texture of the blurred transmittance, or on a light table of the blurred
  // display light, then the fused point stages.
  std::optional<Lease> temporary_flare;
  MTL::Texture*        flare       = unused;
  const bool           light_table = params.light_table != 0U;
  if (image.flare_active) {
    const auto extent = image.flare_extent;
    const Size cells  = MakeSize(extent);
    const auto id     = light_table ? FilmLoupeFlareResultId(plan.drt.node_id)
                                    : FilmFlareResultId(plan.drt.node_id);
    flare             = ReadOrBuildCanvasResult(
        device, plan, id, extent, long_edge, temporary_flare, [&](MTL::Texture* target) {
          const Plane reduced = AllocatePlane(device, canvas.width, canvas.height, PlaneForm::Half);
          CanvasReduce(recording, a, size, reduced, canvas, image.canvas_to_render,
                                   image.canvas_samples,
                       light_table ? CanvasValue::kDisplayLight : CanvasValue::kTransmittance,
                                   print_rgba, params);
          const Plane middle = AllocatePlane(device, extent.width, extent.height, PlaneForm::Half);
          // A canvas with a small flare sigma has no second reduction.
          Plane       source = reduced;
          if (extent.width != canvas.width || extent.height != canvas.height) {
            source = AllocatePlane(device, extent.width, extent.height, PlaneForm::Half);
            Dispatch(device, "film_print_box_reduce")
                .Buffer(0, reduced)
                .Buffer(1, source)
                .Bytes(2, canvas_size)
                .Bytes(3, cells)
                .Run(extent.width, extent.height);
          }
          BlurHorizontal(recording, PlaneForm::Half, PlaneForm::Half, source, middle, cells,
                                     image.flare_x);
          BlurVerticalToTexture(recording, middle, target, cells, image.flare_y);
        });
    names.Mark("FP6 flare canvas");
  }
  DisplayArguments arguments;
  arguments.render_to_flare = image.render_to_flare;
  arguments.flare_strength  = image.flare;
  arguments.has_flare       = image.flare_active ? 1 : 0;
  arguments.has_soft        = soft.has_value() ? 1 : 0;
  arguments.soft_half_size  = image.inhibitor_half_size ? 1 : 0;
  arguments.soft_cells      = MakeSize(image.inhibitor_extent);
  Dispatch(device, "film_print_display")
      .Buffer(0, a)
      .Buffer(1, print_rgba)
      .Buffer(2, soft.has_value() ? *soft : a)
      .Bytes(3, params)
      .Bytes(4, size)
      .Bytes(5, arguments)
      .Texture(0, Native(scene_image))
      .Texture(1, Native(MetalSceneTexture(device, display)))
      .Texture(2, flare)
      .Buffer(6, alpha_mark)
      .Bytes(7, stamp)
      .Run(width, height);
  names.Mark("FP6 display");
}

}  // namespace alcedo
