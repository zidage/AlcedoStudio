#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

auto ReadSourceFile(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  EXPECT_TRUE(file.is_open()) << path.string();
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

auto SourcePath(const std::filesystem::path& relative) -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_SOURCE_ROOT) / relative;
}

void ExpectContains(const std::string& haystack, const char* needle) {
  EXPECT_NE(haystack.find(needle), std::string::npos) << needle;
}

void ExpectNotContains(const std::string& haystack, const char* needle) {
  EXPECT_EQ(haystack.find(needle), std::string::npos) << needle;
}

}  // namespace

TEST(ToneMappingOwnershipTest, CudaLocalToneStageLivesInToneMappingHeader) {
  const auto color = ReadSourceFile(SourcePath(
      "include/edit/operators/GPU_kernels/color.cuh"));
  const auto tone_mapping = ReadSourceFile(SourcePath(
      "include/edit/operators/GPU_kernels/tone_mapping.cuh"));

  ExpectContains(color, "#include \"edit/operators/GPU_kernels/tone_mapping.cuh\"");
  ExpectNotContains(color, "struct GPU_HighlightShadowLocalToneStage");
  ExpectNotContains(color, "HsBuildRemappedSampleKernel");
  ExpectContains(tone_mapping, "struct GPU_HighlightShadowLocalToneStage");
  ExpectContains(tone_mapping, "HsBuildRemappedSampleKernel");
}

TEST(ToneMappingOwnershipTest, OpenClLocalToneHelpersLiveInToneMappingShader) {
  const auto color = ReadSourceFile(SourcePath("edit/pipeline/opencl_shader/color.cl"));
  const auto tone_mapping =
      ReadSourceFile(SourcePath("edit/pipeline/opencl_shader/tone_mapping.cl"));

  ExpectNotContains(color, "opencl_hs_apply_reference_curve");
  ExpectNotContains(color, "opencl_hs_apply_local_tone_pixel");
  ExpectContains(tone_mapping, "opencl_hs_apply_reference_curve");
  ExpectContains(tone_mapping, "opencl_hs_apply_local_tone_pixel");
}

TEST(ToneMappingOwnershipTest, MetalLocalToneHelpersLiveInToneMappingShader) {
  const auto color =
      ReadSourceFile(SourcePath("edit/operators/GPU_kernels/metal_shader/color.metal"));
  const auto tone_mapping =
      ReadSourceFile(SourcePath("edit/operators/GPU_kernels/metal_shader/tone_mapping.metal"));

  ExpectContains(color, "#include \"tone_mapping.metal\"");
  ExpectNotContains(color, "metal_hs_apply_reference_curve");
  ExpectNotContains(color, "GPU_HighlightShadowLocalToneOpKernel");
  ExpectContains(tone_mapping, "metal_hs_apply_reference_curve");
  ExpectContains(tone_mapping, "GPU_HighlightShadowLocalToneOpKernel");
}

TEST(ToneMappingOwnershipTest, RoiMaskMappingKeepsReferenceExtentWhenPatchIsLarger) {
  const auto cuda =
      ReadSourceFile(SourcePath("include/edit/operators/GPU_kernels/tone_mapping.cuh"));
  const auto opencl =
      ReadSourceFile(SourcePath("edit/pipeline/opencl_shader/edit_pipeline_detail.cl"));
  const auto metal = ReadSourceFile(SourcePath("edit/pipeline/metal_shader/fused_pipeline.metal"));

  ExpectContains(cuda, "max(params.render_roi_reference_width_, 1)");
  ExpectContains(cuda, "max(params.render_roi_reference_height_, 1)");
  ExpectNotContains(cuda, "max(params.render_roi_reference_width_, width)");
  ExpectNotContains(cuda, "max(params.render_roi_reference_height_, height)");

  ExpectContains(opencl, "max(params->render_roi_reference_width_, 1)");
  ExpectContains(opencl, "max(params->render_roi_reference_height_, 1)");
  ExpectNotContains(opencl, "max(params->render_roi_reference_width_, width)");
  ExpectNotContains(opencl, "max(params->render_roi_reference_height_, height)");

  ExpectContains(metal, "max(fused_params.render_roi_reference_width_, 1)");
  ExpectContains(metal, "max(fused_params.render_roi_reference_height_, 1)");
  ExpectNotContains(metal, "max(fused_params.render_roi_reference_width_, params.width_)");
  ExpectNotContains(metal, "max(fused_params.render_roi_reference_height_, params.height_)");
}
