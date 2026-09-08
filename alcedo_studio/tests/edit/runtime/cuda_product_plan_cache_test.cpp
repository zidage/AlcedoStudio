//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>
#include <opencv2/core.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/legacy_pipeline_importer.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "image/image_buffer.hpp"
#include "multi_grade_runtime_test_support.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(64, tag);
  bytes[0] = tag;
  bytes[1] = 0x5A;
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern,
                                           gpu_dag_test::DefaultLinearization(),
                                           gpu_dag_test::FullSensor(32, 32), decode_res);
  };
}

auto RenderHost(CudaProductRenderer& renderer, const std::shared_ptr<ImageBuffer>& input,
                DecodeRes decode_res, const RenderRequest& request)
    -> std::shared_ptr<ImageBuffer> {
  return renderer.Render(input, decode_res, request, nullptr, FrameCompletionSubmission{}, true);
}

TEST(GpuDagCudaDrtProduct, ProductRendererCompilesStaticPlanOnlyForTopologyOrSourceLayoutChange) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document);
  CudaProductRenderer renderer(document, MakeUnpacker());
  const auto          image = MakeEncodedImage(11);
  RenderRequest       request;

  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);
  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 1U);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 1U);

  auto* exposure = dynamic_cast<ExposureModel*>(
      document->PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure()));
  ASSERT_NE(exposure, nullptr);
  exposure->SetValue(0.8f);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  auto develop       = document->Develop()->Params().Params();
  develop.wb_mode    = "custom";
  develop.custom_cct = 4800.0f;
  document->Develop()->Params().ReplaceParams(develop);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  auto drt           = document->Drt()->Params().Params();
  drt.peak_luminance = 180.0f;
  document->Drt()->Params().ReplaceParams(drt);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  request.resolution.quality = RenderQuality::Export;
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  document->Geometry().SetCropRect({0.05f, 0.05f, 0.9f, 0.9f});

  auto&      encoded       = image->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  const auto    source = renderer.SourceCache().AcquireEncoded(encoded_bytes, DecodeRes::FULL);
  auto          plan   = renderer.PlanCache().GetOrCompile(*document, source.Get().CompileSource());
  const auto    key    = plan.static_key;
  RenderRequest viewport                   = request;
  viewport.view.visible_rect_in_edit_space = {0.1f, 0.1f, 0.8f, 0.8f};
  viewport.view.viewport_extent            = {24, 24};
  GraphCompiler::BindFrameGeometry(plan, *document, viewport);
  EXPECT_EQ(plan.static_key, key);
  EXPECT_NE(plan.geometry.render_extent.width, 0U);
  document->Geometry().SetCropRect({});

  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 1U);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 1U);
  EXPECT_EQ(renderer.Stats().prepared_source_misses, 1U);
  EXPECT_GE(renderer.Stats().prepared_source_hits, 4U);
  EXPECT_GE(renderer.Stats().plan_cache_hits, 4U);

  auto* grade = document->PrimaryGrade();
  grade->MoveAdjustment(grade->AdjustmentIdAt(0), grade->AdjustmentCount() - 1);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 2U);
  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 1U);

  grade_mask_test::AddRadialMask(*document, MaskId{"mask.radial"});
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 3U);

  ASSERT_NE(RenderHost(renderer, image, DecodeRes::HALF, request), nullptr);
  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 2U);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 4U);

  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);
  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 2U);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 4U);
}

TEST(GpuDagCudaDrtProduct, ProductRendererReusesPreparedSourceAfterSwitchingEncodedBuffers) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document);
  CudaProductRenderer renderer(document, MakeUnpacker());
  const auto          image_a = MakeEncodedImage(21);
  const auto          image_b = MakeEncodedImage(22);
  RenderRequest       request;

  ASSERT_NE(RenderHost(renderer, image_a, DecodeRes::FULL, request), nullptr);
  ASSERT_NE(RenderHost(renderer, image_b, DecodeRes::FULL, request), nullptr);
  ASSERT_NE(RenderHost(renderer, image_a, DecodeRes::FULL, request), nullptr);

  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 2U);
  EXPECT_EQ(renderer.Stats().prepared_source_misses, 2U);
  EXPECT_EQ(renderer.Stats().prepared_source_hits, 1U);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 1U);
}

TEST(GpuDagCudaDrtProduct,
     ProductRendererViewportAndMaxEdgeResampleDecodedSourceWithoutSizeMismatch) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document);
  CudaProductRenderer renderer(document, MakeUnpacker());
  const auto          image = MakeEncodedImage(31);
  RenderRequest       request;
  request.view.viewport_extent = {48, 32};
  request.resolution.max_edge  = 48;

  const auto output            = RenderHost(renderer, image, DecodeRes::FULL, request);
  ASSERT_NE(output, nullptr);
  const auto& cpu = output->GetCPUData();
  EXPECT_EQ(cpu.cols, 48);
  EXPECT_EQ(cpu.rows, 32);

  request.view.visible_rect_in_edit_space = {0.1f, 0.1f, 0.8f, 0.8f};
  request.view.viewport_extent            = {40, 24};
  const auto cropped                      = RenderHost(renderer, image, DecodeRes::FULL, request);
  ASSERT_NE(cropped, nullptr);
  // Full-frame may upsample (48x32 from 24x24 develop). A visible subregion must
  // not manufacture a larger patch: 0.8 * 24 native ROI rounds to 19x19.
  EXPECT_EQ(cropped->GetCPUData().cols, 19);
  EXPECT_EQ(cropped->GetCPUData().rows, 19);
}

TEST(GpuDagCudaDrtProduct, ProductRendererRendersLegacyImportWithTintWithoutUnregisteredType) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

  nlohmann::json legacy;
  legacy["Color Adjustment"]["Color Adjustment"]["tint"] = {
      {"type", 11}, {"enable", true}, {"params", {{"tint", 18.0f}}}};
  auto imported = LegacyPipelineImporter::Import(legacy);
  ASSERT_TRUE(imported.Ok()) << imported.error;
  ASSERT_EQ(imported.document->PrimaryGrade()->FindAdjustmentByType(type_ids::Tint()), nullptr);
  const auto* cat02 = dynamic_cast<const Cat02WhiteBalanceModel*>(
      imported.document->PrimaryGrade()->FindAdjustmentByType(type_ids::Cat02WhiteBalance()));
  ASSERT_NE(cat02, nullptr);
  EXPECT_FLOAT_EQ(cat02->TintOffset(), 18.0f);

  auto document = std::make_shared<PipelineDocument>(std::move(*imported.document));
  gpu_dag_test::EnsureTestCameraProfile(*document);
  CudaProductRenderer renderer(document, MakeUnpacker());
  const auto          image = MakeEncodedImage(41);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, RenderRequest{}), nullptr);
}

TEST(GpuDagCudaDrtProduct, LegacyShadowControlExecutesLocalLaplacianWorkspacePath) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";

  nlohmann::json legacy;
  legacy["Basic Adjustment"]["Basic Adjustment"]["shadows"] = {
      {"type", 6}, {"enable", true}, {"params", {{"shadows", 60.0f}}}};
  auto imported = LegacyPipelineImporter::Import(legacy);
  ASSERT_TRUE(imported.Ok()) << imported.error;
  auto document = std::make_shared<PipelineDocument>(std::move(*imported.document));
  gpu_dag_test::EnsureTestCameraProfile(*document);

  CudaProductRenderer renderer(document, MakeUnpacker());
  ASSERT_NE(RenderHost(renderer, MakeEncodedImage(42), DecodeRes::FULL, RenderRequest{}), nullptr);
  const auto* reference = renderer.Device().Workspace().Images().Find(
      LocalToneSourceId(document->PrimaryGrade()->Id()));
  ASSERT_NE(reference, nullptr);
  EXPECT_GT(reference->Texture().Bytes(), sizeof(float));
}

void ExpectMatchingPixels(ImageBuffer& cached, ImageBuffer& fresh) {
  const auto& actual = cached.GetCPUData();
  const auto& expected = fresh.GetCPUData();
  ASSERT_FALSE(actual.empty());
  ASSERT_EQ(actual.size(), expected.size());
  ASSERT_EQ(actual.type(), expected.type());
  ASSERT_EQ(actual.depth(), CV_32F);
  ASSERT_TRUE(cv::checkRange(actual));
  ASSERT_TRUE(cv::checkRange(expected));
  EXPECT_LE(cv::norm(actual, expected, cv::NORM_INF), 1.0e-5);
}

TEST(GpuDagCudaDrtProduct,
     RepeatedFullCropAndExposureEditsBoundTextureEntriesAndBytesAndPreserveSourceHits) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document);
  CudaProductRenderer renderer(document, MakeUnpacker());
  const auto image = MakeEncodedImage(51);
  RenderRequest request;
  auto& exposure = multi_grade_test::GradeAdjustment<ExposureModel>(
      *document, document->PrimaryGrade()->Id(), type_ids::Exposure());
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);
  // Two complete full-size frame working sets allow current results and warm scratch.
  // The bound is fixed before edits; neither iteration count nor visited sizes enters it.
  const auto max_entries = 2U * renderer.Device().Workspace().Textures().EntryCount();
  const auto max_bytes = 2U * renderer.Device().Workspace().Textures().UsedBytes();
  for (int iteration = 0; iteration < 48; ++iteration) {
    SCOPED_TRACE(iteration);
    const float side = static_cast<float>(8 + (iteration * 7) % 17) / 24.0f;
    document->Geometry().SetCropRect({0.0f, 0.0f, side, side});
    for (int edit = 0; edit < 2; ++edit) {
      SCOPED_TRACE(edit);
      exposure.SetValue(static_cast<float>((iteration * 2 + edit) % 9) * 0.15f);
      renderer.Device().ResetPassStats();
      const auto cached = RenderHost(renderer, image, DecodeRes::FULL, request);
      ASSERT_NE(cached, nullptr);
      EXPECT_EQ(renderer.Device().PassStats().sensor_develop_execute, 0U);
      EXPECT_EQ(renderer.Device().PassStats().sensor_develop_skip, 1U);
      EXPECT_EQ(renderer.Device().PassStats().source_h2d_count, 0U);
      EXPECT_LE(renderer.Device().Workspace().Textures().EntryCount(), max_entries);
      EXPECT_LE(renderer.Device().Workspace().Textures().UsedBytes(), max_bytes);
      CudaProductRenderer fresh(document, MakeUnpacker());
      const auto expected = RenderHost(fresh, image, DecodeRes::FULL, request);
      ASSERT_NE(expected, nullptr);
      ExpectMatchingPixels(*cached, *expected);
    }
  }
  EXPECT_EQ(renderer.Stats().libraw_open_unpack_count, 1U);
  EXPECT_EQ(renderer.Stats().prepared_source_misses, 1U);
  EXPECT_EQ(renderer.Stats().prepared_source_hits, 96U);
  EXPECT_EQ(renderer.Stats().plan_compile_count, 1U);
}

TEST(GpuDagCudaDrtProduct,
     MultipleFullGradesReuseOnePingPongWorkingSetWithLocalToneAndExposure) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  const auto unpack = [](std::span<const std::byte>, DecodeRes decode_res) {
    EXPECT_EQ(decode_res, DecodeRes::FULL);
    return RawInputLoader::FromDirectRgb(
        multi_grade_test::MakeNeighborhoodRgbaPlane(64, 64, 0.02f, 0.08f),
        gpu_dag_test::FullSensor(64, 64));
  };
  auto configure_grade = [](PipelineDocument& document, const NodeId& id) {
    multi_grade_test::GradeAdjustment<ExposureModel>(document, id, type_ids::Exposure())
        .SetValue(0.4f);
    // Shadows is the Grade-owned neighborhood operation. Clarity/Sharpen belong to DRT.
    // Pointwise + Local Laplacian + final mix require both Ping and Pong destinations.
    multi_grade_test::GradeAdjustment<ShadowsModel>(document, id, type_ids::Shadows())
        .SetValue(40.0f);
    multi_grade_test::GradeNode(document, id.Value())->SetMix(0.75f);
  };
  auto single = std::make_shared<PipelineDocument>(multi_grade_test::MakeIdentityGradeDocument());
  configure_grade(*single, NodeId{"grade.primary"});
  const auto image = MakeEncodedImage(52);
  const RenderRequest request;
  CudaProductRenderer single_renderer(single, unpack);
  ASSERT_NE(RenderHost(single_renderer, image, DecodeRes::FULL, request), nullptr);
  const auto single_entries = single_renderer.Device().Workspace().Textures().EntryCount();
  const auto single_bytes = single_renderer.Device().Workspace().Textures().UsedBytes();
  const auto single_published = single_renderer.Device().Workspace().Images().PublishedCount();

  auto document = std::make_shared<PipelineDocument>(multi_grade_test::MakeIdentityGradeDocument());
  multi_grade_test::AddCleanGradesBeforeDrt(*document, {"grade.b", "grade.c", "grade.d"});
  for (const char* id : {"grade.primary", "grade.b", "grade.c", "grade.d"}) {
    configure_grade(*document, NodeId{id});
  }
  CudaProductRenderer renderer(document, unpack);
  const auto cached = RenderHost(renderer, image, DecodeRes::FULL, request);
  ASSERT_NE(cached, nullptr);
  EXPECT_EQ(renderer.Device().PassStats().primary_grade_execute, 4U);
  // Persistent Grade outputs and LLF source/result planes are legitimate extra storage.
  // All other storage must fit the same single-Grade working set, including warm idle entries.
  const auto published = renderer.Device().Workspace().Images().PublishedCount();
  ASSERT_GE(published, single_published);
  const auto extra_results = published - single_published;
  EXPECT_LE(renderer.Device().Workspace().Textures().EntryCount(), single_entries + extra_results);
  EXPECT_LE(renderer.Device().Workspace().Textures().UsedBytes(),
            single_bytes + extra_results * 64U * 64U * 4U * sizeof(float));
  CudaProductRenderer fresh(document, unpack);
  const auto expected = RenderHost(fresh, image, DecodeRes::FULL, request);
  ASSERT_NE(expected, nullptr);
  ExpectMatchingPixels(*cached, *expected);
}

}  // namespace
}  // namespace alcedo
