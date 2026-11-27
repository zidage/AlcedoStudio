//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Develop pass of the Vulkan backend against the Develop pass of a reference backend on the
// same GPU (#1036, phase VK6, decision B9). The reference backend is OpenCL when the build has
// OpenCL (Windows), else Metal (macOS, where the build has no OpenCL). Both sides run the product
// functions on the same prepared input and compare `develop.sensor_linear`.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using namespace vulkan_reference;

constexpr const char* kClippedBayerRaw = "raw/street/_DSC0155.NEF";
constexpr const char* kXTransRaw       = "raw/xtrans_edge/DSCF0019.RAF";
constexpr const char* kLinearDng       = "raw/linear_dng/mfzoty.dng";
constexpr const char* kWarpDng =
    "tag @ryanbreitkreutz - free raws from @signatureeditsco - DSC06683.dng";

/// The full-size prepared input of @p path, decoded one time for the run. Null when it is absent.
auto LoadEncoded(const std::filesystem::path& path) -> std::shared_ptr<const PreparedRawInput> {
  static std::map<std::string, std::shared_ptr<const PreparedRawInput>> cache;
  if (const auto it = cache.find(path.string()); it != cache.end()) {
    return it->second;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return nullptr;
  }
  const std::vector<char> chars((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  auto input = std::make_shared<const PreparedRawInput>(
      RawInputLoader::LoadEncoded(std::span<const std::byte>(bytes), DecodeRes::FULL));
  cache[path.string()] = input;
  return input;
}

auto MakeDocument(bool highlights_reconstruct) -> PipelineDocument {
  auto document                  = CreateDefaultPipelineDocument();
  auto payload                   = document.Develop()->Params().Params();
  payload.demosaic_method        = "legacy";
  payload.highlights_reconstruct = highlights_reconstruct;
  document.Develop()->Params().ReplaceParams(std::move(payload));
  return document;
}

/// One Develop on a new render device of @p Device; returns `develop.sensor_linear`.
template <class Device, class Execute>
auto RenderDevelop(const PipelineDocument& document, const PreparedRawInput& input, Execute execute)
    -> std::vector<float> {
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  Device     device;
  device.BeginRender();
  execute(device, plan, input, document);
  device.EndRender();
  device.WaitIdle();
  return DownloadImage(device, plan.sensor_linear_output);
}

/// The width of `develop.sensor_linear` of @p input.
auto DevelopWidth(const PreparedRawInput& input) -> std::uint32_t {
  return GraphCompiler::Compile(CreateDefaultPipelineDocument(), input.CompileSource(),
                                RenderRequest{})
      .source.develop_output_extent.width;
}

auto RenderVulkan(const PipelineDocument& document, const PreparedRawInput& input)
    -> std::vector<float> {
  return RenderDevelop<VulkanRenderDevice>(document, input, ExecuteVulkanDevelop);
}

auto RenderReference(const PipelineDocument& document, const PreparedRawInput& input)
    -> std::vector<float> {
  return RenderDevelop<ReferenceRenderDevice>(document, input, ExecuteReferenceDevelop);
}

class VulkanDevelopTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }

  static auto RequireSample(const char* relative) -> std::shared_ptr<const PreparedRawInput> {
    auto input = LoadEncoded(std::filesystem::path(TEST_IMG_PATH) / relative);
    EXPECT_NE(input, nullptr) << "test image is missing: " << relative;
    return input;
  }

  /// The Develop of @p input on both backends, with highlight reconstruction off and on.
  static void ExpectBothHighlightModesEqual(const std::string& label, const PreparedRawInput& input,
                                            const Tolerance& tolerance) {
    const std::uint32_t width = DevelopWidth(input);
    for (const bool hlr : {false, true}) {
      const auto document = MakeDocument(hlr);
      const auto vulkan   = RenderVulkan(document, input);
      const auto expected = RenderReference(document, input);
      ExpectEqualToReference(label + (hlr ? ", highlight reconstruction" : ""), expected, vulkan,
                             width, tolerance);
    }
  }
};

// The tolerances of the measurement of this phase (plan, VK6 completion record).
#if defined(HAVE_OPENCL)
/// The demosaic and highlight cases against OpenCL (B9): the RCD tolerance of VK4.
constexpr Tolerance kDemosaicTolerance = Uniform(1.0e-4);
#else
/// The demosaic and highlight cases against Metal. The Metal RAW kernels have fast math and
/// contract products into FMAs, the OpenCL and Vulkan kernels do not: on the Apple M4 the RCD and
/// X-Trans results of Metal differ from Vulkan by up to 0.47 (rms 1.3e-4) at a few edges of the
/// image. With Metal built without fast math and contraction, the two were equal bit for bit
/// (VK6 record). The bound is the root mean square; the largest value only catches a gross error.
constexpr Tolerance kDemosaicTolerance = {0.5, 0.5, 0, 2.5e-4};
#endif
constexpr Tolerance kDevelopTolerance = Uniform(1.0e-4);
constexpr Tolerance kLensTolerance    = Uniform(5.0e-4);
/// 1e-5 inside (the DNG warp tolerance of VK5); 8 ulp of a coordinate below 16384 in the border
/// band, where the bilinear footprint reaches outside the source and a value difference is the
/// position difference (VK5 record).
constexpr Tolerance kWarpTolerance    = {1.0e-5, 8.0 * 0x1p-10, 16};

TEST_F(VulkanDevelopTest, BayerRawEqualsTheReferenceBackend) {
  const auto input = RequireSample(kClippedBayerRaw);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->cfa_pattern.kind, RawCfaKind::Bayer2x2);
  ExpectBothHighlightModesEqual("Bayer NEF", *input, kDemosaicTolerance);
}

TEST_F(VulkanDevelopTest, XTransRawEqualsTheReferenceBackend) {
  const auto input = RequireSample(kXTransRaw);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->cfa_pattern.kind, RawCfaKind::XTrans6x6);
  ExpectBothHighlightModesEqual("X-Trans RAF", *input, kDemosaicTolerance);
}

TEST_F(VulkanDevelopTest, LinearDngEqualsTheReferenceBackend) {
  const auto input = RequireSample(kLinearDng);
  ASSERT_NE(input, nullptr);
  ASSERT_EQ(input->input_kind, RawInputKind::DebayeredRgb);
  ExpectBothHighlightModesEqual("linear DNG", *input, kDevelopTolerance);
}

TEST_F(VulkanDevelopTest, DngWarpOfARealDngEqualsTheReferenceBackend) {
  // The OpcodeList3 WarpRectilinear of a real DNG on a position ramp of its sensor size (plan
  // section 11): `develop.sensor_unwarped` to `develop.sensor_linear` in the Develop pass.
  const auto dng = LoadEncoded(std::filesystem::path(ALCEDO_CI_RAW_FIXTURE_ROOT) / kWarpDng);
  ASSERT_NE(dng, nullptr) << "test image is missing: " << kWarpDng;
  ASSERT_TRUE(dng->dng_warp_rectilinear.has_value());
  const std::uint32_t width  = dng->host_extent.width;
  const std::uint32_t height = dng->host_extent.height;
  auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(width, height),
                                             gpu_dag_test::FullSensor(width, height));
  input.dng_warp_rectilinear = dng->dng_warp_rectilinear;
  const auto document        = MakeDocument(false);
  const auto vulkan          = RenderVulkan(document, input);
  const auto expected        = RenderReference(document, input);
  ExpectEqualToReference("WarpRectilinear of a real DNG (" +
                             std::to_string(dng->dng_warp_rectilinear->coefficient_set_count) +
                             " sets)",
                         expected, vulkan, width, kWarpTolerance);
}

TEST_F(VulkanDevelopTest, LensCalibrationEqualsTheReferenceBackend) {
  // A position ramp (R = (x + 0.5) / width, G = (y + 0.5) / height; plan section 11) with the
  // Lensfun profile of the OpenCL vignetting test: a value difference is a position difference.
  constexpr std::uint32_t kWidth  = 640;
  constexpr std::uint32_t kHeight = 480;
  auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(kWidth, kHeight),
                                             gpu_dag_test::FullSensor(kWidth, kHeight));
  input.color_context.valid_               = true;
  input.color_context.lens_metadata_valid_ = false;
  input.color_context.focal_length_mm_     = 32.0f;
  input.color_context.aperture_f_number_   = 1.8f;
  input.color_context.focus_distance_m_    = 10.0f;
  input.color_context.crop_factor_hint_    = 1.534f;

  const auto plain                         = MakeDocument(false);
  auto       document                      = MakeDocument(false);
  auto       payload                       = document.Develop()->Params().Params();
  payload.lens_enabled                     = true;
  payload.apply_vignetting                 = true;
  payload.apply_distortion                 = true;
  payload.apply_tca                        = true;
  payload.apply_crop                       = false;
  payload.projection_enabled               = false;
  payload.lens_maker                       = "Zeiss";
  payload.lens_model                       = "Touit 1.8/32";
  payload.lens_profile_db_path             = ALCEDO_LENS_PROFILE_DB_DIR;
  document.Develop()->Params().ReplaceParams(std::move(payload));
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  ASSERT_TRUE(plan.Contains(GpuPassKind::Lens));

  const auto vulkan   = RenderVulkan(document, input);
  const auto expected = RenderReference(document, input);
  const auto without  = RenderVulkan(plain, input);
  ASSERT_EQ(vulkan.size(), without.size());
  // The lens stage ran.
  EXPECT_GT(Compare(without, vulkan, Uniform(1.0e-3)).above, 0U);
  ExpectEqualToReference("lens vignetting, distortion, TCA", expected, vulkan, kWidth,
                         kLensTolerance);
}

TEST_F(VulkanDevelopTest, DevelopRecordsIntoOneRecordingWithoutASubmission) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto input   = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(96, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(96, 64), DecodeRes::FULL);
  const auto document = MakeDocument(true);
  const auto plan     = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  backend.ResetCounters();
  device.BeginRender();
  PassEncoder<VulkanBackend, GpuPassKind::UploadRaw>::Encode(device, plan, input, document);
  EXPECT_EQ(backend.SubmitCount(), 0U);
  EXPECT_EQ(backend.WaitCount(), 0U);
  EXPECT_TRUE(device.CommandContext().IsRecording());
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(backend.SubmitCount(), 1U);
  const auto pixels = DownloadImage(device, plan.sensor_linear_output);
  ASSERT_EQ(pixels.size(), static_cast<std::size_t>(plan.source.develop_output_extent.width) *
                               plan.source.develop_output_extent.height * 4U);
  EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](float v) { return std::isfinite(v); }));
}

TEST_F(VulkanDevelopTest, ScratchIsReleasedAndTheOutputIsReusedByASecondDevelop) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto input   = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(96, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(96, 64), DecodeRes::FULL);
  const auto document = MakeDocument(true);
  const auto plan     = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  const auto         before  = backend.AllocationTotals();
  backend.ResetCounters();

  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  // The output texture; the buffers: the CFA upload, the linear plane, the parameters, five RCD
  // planes, two highlight masks, the statistics.
  EXPECT_EQ(backend.TextureCreateCount(), 1U);
  EXPECT_EQ(backend.BufferCreateCount(), 11U);
  // PlanExecutor waits after the Develop pass; then only the output is live.
  backend.SynchronizeRecordedWork(device.CommandContext());
  EXPECT_EQ(backend.RetiredResourceCount(), 0U);
  EXPECT_EQ(backend.AllocationTotals().live_allocations, before.live_allocations + 1U);
  device.EndRender();
  device.WaitIdle();
  const auto first = DownloadImage(device, plan.sensor_linear_output);

  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(backend.TextureCreateCount(), 1U) << "the second Develop creates no output texture";
  EXPECT_EQ(DownloadImage(device, plan.sensor_linear_output), first);
}

TEST_F(VulkanDevelopTest, NeuralEngineAndRasterPixelsWithoutRasterInputFail) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto cfa     = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(96, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(96, 64), DecodeRes::FULL);
  auto neural             = MakeDocument(false);
  auto payload            = neural.Develop()->Params().Params();
  payload.demosaic_method = "neural_engine";
  neural.Develop()->Params().ReplaceParams(std::move(payload));
  const auto         plan = GraphCompiler::Compile(neural, cfa.CompileSource(), RenderRequest{});

  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  backend.ResetCounters();
  device.BeginRender();
  try {
    ExecuteVulkanDevelop(device, plan, cfa, neural);
    ADD_FAILURE() << "the Neural Engine request did not fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("Neural Engine"), std::string::npos) << error.what();
    EXPECT_NE(std::string(error.what()).find("Vulkan"), std::string::npos) << error.what();
  }

  auto raster       = cfa;
  raster.input_kind = RawInputKind::RasterRgb;
  try {
    ExecuteVulkanDevelop(device, plan, raster, MakeDocument(false));
    ADD_FAILURE() << "raster pixels of a document without a raster input did not fail";
  } catch (const std::runtime_error& error) {
    // Phase VK7: the raster input runs; the document needs its raster input object.
    EXPECT_NE(std::string(error.what()).find("no raster input"), std::string::npos) << error.what();
  }
  EXPECT_FALSE(device.CommandContext().IsRecording());
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  device.CancelRender();
}

TEST_F(VulkanDevelopTest, InvalidRequestIsRejectedBeforeACommand) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto input   = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(96, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(96, 64), DecodeRes::FULL);
  const auto document = MakeDocument(false);
  const auto plan     = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});

  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  backend.ResetCounters();
  // Outside BeginRender.
  EXPECT_THROW(ExecuteVulkanDevelop(device, plan, input, document), std::runtime_error);

  device.BeginRender();
  auto padded                = input;
  padded.pixels.stride_bytes = (96 + 2) * sizeof(std::uint16_t);
  EXPECT_THROW(ExecuteVulkanDevelop(device, plan, padded, document), std::runtime_error);
  auto empty              = input;
  empty.host_extent.width = 0;
  EXPECT_THROW(ExecuteVulkanDevelop(device, plan, empty, document), std::runtime_error);
  PipelineDocument no_develop;
  EXPECT_THROW(ExecuteVulkanDevelop(device, plan, input, no_develop), std::runtime_error);
  EXPECT_FALSE(device.CommandContext().IsRecording());
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  EXPECT_EQ(backend.TextureCreateCount(), 0U);
  device.CancelRender();
}

}  // namespace
}  // namespace alcedo
