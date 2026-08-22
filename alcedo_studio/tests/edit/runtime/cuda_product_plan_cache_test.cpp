//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "image/image_buffer.hpp"

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
    return RawInputLoader::FromUnpackedCfa(
        gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern, gpu_dag_test::DefaultLinearization(),
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

  auto develop = document->Develop()->Params().Params();
  develop.wb_mode    = "custom";
  develop.custom_cct = 4800.0f;
  document->Develop()->Params().ReplaceParams(develop);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  auto drt = document->Drt()->Params().Params();
  drt.peak_luminance = 180.0f;
  document->Drt()->Params().ReplaceParams(drt);
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  request.resolution.quality = RenderQuality::Export;
  ASSERT_NE(RenderHost(renderer, image, DecodeRes::FULL, request), nullptr);

  document->Geometry().SetCropRect({0.05f, 0.05f, 0.9f, 0.9f});

  auto& encoded = image->GetBuffer();
  const auto encoded_bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte*>(encoded.data()), encoded.size()};
  const auto source = renderer.SourceCache().AcquireEncoded(encoded_bytes, DecodeRes::FULL);
  auto       plan   = renderer.PlanCache().GetOrCompile(*document, source.Get().CompileSource());
  const auto key    = plan.static_key;
  RenderRequest viewport = request;
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

  document->Graph().AddNode(std::make_unique<AnalyticMaskNodeModel>(NodeId{"mask.radial"}));
  document->Graph().Connect(NodeId{"mask.radial"}, PortId{"mask"}, NodeId{"grade.primary"},
                            PortId{"mask"});
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

}  // namespace
}  // namespace alcedo
