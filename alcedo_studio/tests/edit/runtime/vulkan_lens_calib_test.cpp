//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Vulkan lens calibration of the Develop pass (plan phase VK5): the three cases of
// opencl_lens_calib_test.cpp against the CPU reference of lens_warp_test_support.hpp, and the
// projections and crops against the product OpenCL path on the same GPU (decision B9):
// `OpenCL::Geometry::ApplyLensCalibration`, then the resize back to the image size of
// `ExecuteOpenClLensCalibration`. Each test runs with the validation layer. Set
// ALCEDO_VULKAN_DEVICE (for example "Iris") to select the device; the OpenCL device follows it.

#include "edit/runtime/lens/vulkan/vulkan_lens_calib.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <opencv2/core.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../../vulkan/vulkan_opencl_comparison.hpp"
#include "edit/runtime/lens/opencl/opencl_lens_calib_ops.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_render_device.hpp"
#include "image/opencl_image.hpp"
#include "lens_warp_test_support.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_geometry_utils.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using vulkan_opencl_comparison::DifferenceRule;
using vulkan_opencl_comparison::ExpectEqualWithinTolerance;

/// Vulkan against OpenCL, absolute, on the position ramp of @ref PositionRamp, for every texel.
/// Where the sample is inside the source, a value difference is the position difference divided
/// by the image size. Near the source border it is larger: within one pixel the border coverage
/// makes it the position difference itself, and within three pixels the taps that repeat the
/// edge texel make the 2 x 2 clamp act. 5e-4 is 8 ulp of a position of 640 pixels. Measured (VK5
/// record): 3.7e-4 at the coverage border after a projection (4 to 6 ulp of the position).
constexpr double kLensTolerance         = 5.0e-4;
/// The texels whose Lanczos footprint is inside the source: a position difference of 1e-5 times
/// the image size (0.0064 pixel).
constexpr double kLensInteriorTolerance = 1.0e-5;

class VulkanLensCalibTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = vulkan_opencl_comparison::PrepareOpenClOnTheVulkanDevice();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

auto Values(const cv::Mat& image) -> std::vector<float> {
  const cv::Mat packed = image.isContinuous() ? image : image.clone();
  return {packed.ptr<float>(0), packed.ptr<float>(0) + packed.total() * 4};
}

/**
 * @brief RGBA32F of the source position: R = (x + 0.5) / width, G = (y + 0.5) / height,
 * B = (R + G) / 2, alpha 1.
 *
 * The comparison with OpenCL measures the source positions of the warp: on a plane the Lanczos
 * sample is the plane value, so a difference of R or G is a difference of the source position
 * divided by the image size. A pattern with detail is not used here: where the Lanczos sample
 * lies outside the range of its 2 x 2 neighbours, the clamp of the sampler (issue #713) changes
 * the result by up to a few hundredths when the position crosses an integer row or column, and a
 * position difference of 1e-5 pixel at such a crossing gives that change (VK5 record). The three
 * cases of the CPU reference cover the sampler on detail.
 */
auto PositionRamp(int width, int height) -> cv::Mat {
  cv::Mat image(height, width, CV_32FC4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      auto*       pixel = image.ptr<float>(y) + x * 4;
      const float r     = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
      const float g     = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
      pixel[0]          = r;
      pixel[1]          = g;
      pixel[2]          = 0.5F * (r + g);
      pixel[3]          = 1.0F;
    }
  }
  return image;
}

/// The textures of one Vulkan lens calibration of an image of @p width x @p height.
struct LensTextures {
  VulkanBackend::Texture2D image;
  VulkanBackend::Texture2D warped;
  VulkanBackend::Texture2D cropped;
  VulkanBackend::Buffer    parameters;

  auto                     Scratch() -> vulkan::lens::LensCalibrationScratch {
    return {warped.Empty() ? nullptr : &warped, cropped.Empty() ? nullptr : &cropped};
  }
};

/// The scratch of the steps of @p params (the allocation rule of a caller).
auto MakeLensTextures(VulkanBackend& backend, const LensCalibGpuParams& params, int width,
                      int height) -> LensTextures {
  const auto   w     = static_cast<std::uint32_t>(width);
  const auto   h     = static_cast<std::uint32_t>(height);
  const auto   steps = vulkan::lens::ResolveLensCalibrationSteps(params, w, h);
  LensTextures textures;
  textures.image      = backend.CreateTexture2D(w, h, TextureFormat::Rgba32f);
  textures.parameters = backend.CreateBuffer(vulkan::lens::kLensCalibParameterBytes);
  if (steps.warp) {
    textures.warped = backend.CreateTexture2D(w, h, TextureFormat::Rgba32f);
  }
  if (steps.rectangle_crop.has_value()) {
    textures.cropped = backend.CreateTexture2D(
        static_cast<std::uint32_t>(steps.rectangle_crop->width),
        static_cast<std::uint32_t>(steps.rectangle_crop->height), TextureFormat::Rgba32f);
  }
  return textures;
}

/// The Vulkan lens calibration of @p source; the result has the size of the source.
auto RunVulkanLensPass(const cv::Mat& source, const LensCalibGpuParams& params) -> cv::Mat {
  VulkanRenderDevice device;
  auto&              backend  = device.Workspace().Device();
  auto&              context  = device.CommandContext();
  auto               textures = MakeLensTextures(backend, params, source.cols, source.rows);
  const cv::Mat      packed   = source.isContinuous() ? source : source.clone();
  backend.UploadTexture2D(
      textures.image,
      std::as_bytes(std::span<const float>(packed.ptr<float>(0), packed.total() * 4)), context);
  vulkan::lens::EncodeLensCalibration(backend, context, textures.image, textures.Scratch(),
                                      VulkanBufferView{&textures.parameters, 0}, params);
  cv::Mat result(source.rows, source.cols, CV_32FC4);
  backend.DownloadTexture2D(
      textures.image,
      std::as_writable_bytes(std::span<float>(result.ptr<float>(0), result.total() * 4)), context);
  return result;
}

/// The OpenCL path of the Develop pass: `ApplyLensCalibration`, then the resize back to the size
/// of the source (`ExecuteOpenClLensCalibration`).
auto RunOpenClDevelopLensPass(const cv::Mat& source, const LensCalibGpuParams& params) -> cv::Mat {
  opencl::OpenClImage image;
  image.Upload(source);
  const cl_mem uploaded = image.Buffer();
  OpenCL::Geometry::ApplyLensCalibration(image, params);
  if (image.Buffer() != uploaded || image.Width() != source.cols || image.Height() != source.rows) {
    opencl::OpenClImage resized;
    OpenCL::Geometry::ResizeLinear(image, resized, cv::Size(source.cols, source.rows));
    image = std::move(resized);
  }
  if (clFinish(OpenClContext::Instance().Queue()) != CL_SUCCESS) {
    throw std::runtime_error("clFinish failed after the OpenCL lens pass");
  }
  cv::Mat result;
  image.Download(result);
  return result;
}

void ExpectEqualToOpenCl(const std::string& label, const cv::Mat& source,
                         const LensCalibGpuParams& params) {
  const cv::Mat opencl = RunOpenClDevelopLensPass(source, params);
  const cv::Mat vulkan = RunVulkanLensPass(source, params);
  ASSERT_EQ(opencl.size(), vulkan.size()) << label;
  ExpectEqualWithinTolerance(label, Values(opencl), Values(vulkan), kLensTolerance,
                             DifferenceRule::Absolute);
  // The ramp gives the source position of each sample (x = R * width - 0.5). A texel samples
  // inside the source when its 6 x 6 Lanczos footprint (taps -2 to +3) is inside it with one
  // pixel of margin, and its coverage (alpha) is 1 on both backends.
  const auto inside = [&](const cv::Vec4f& sample) {
    const double sx = static_cast<double>(sample[0]) * opencl.cols - 0.5;
    const double sy = static_cast<double>(sample[1]) * opencl.rows - 0.5;
    return sample[3] == 1.0F && sx >= 3.0 && sy >= 3.0 && sx <= opencl.cols - 5.0 &&
           sy <= opencl.rows - 5.0;
  };
  std::vector<float> inside_opencl, inside_vulkan;
  for (int y = 0; y < opencl.rows; ++y) {
    for (int x = 0; x < opencl.cols; ++x) {
      const auto& a = opencl.at<cv::Vec4f>(y, x);
      const auto& b = vulkan.at<cv::Vec4f>(y, x);
      if (inside(a) && b[3] == 1.0F) {
        inside_opencl.insert(inside_opencl.end(), {a[0], a[1], a[2]});
        inside_vulkan.insert(inside_vulkan.end(), {b[0], b[1], b[2]});
      }
    }
  }
  EXPECT_GT(inside_opencl.size(), opencl.total());
  ExpectEqualWithinTolerance(label + " inside the source", inside_opencl, inside_vulkan,
                             kLensInteriorTolerance, DifferenceRule::Absolute);
}

/// The issue #713 profile (distortion, TCA, auto scale) for the test image size.
auto Issue713Params() -> LensCalibGpuParams {
  return lens_warp_test::ResolveIssue713Params(lens_warp_test::kTestWidth,
                                               lens_warp_test::kTestHeight);
}

// ---- The three cases of opencl_lens_calib_test.cpp ------------------------------------------

TEST_F(VulkanLensCalibTest, Issue713GratingKeepsAmplitudeAndMatchesCpuReference) {
  lens_warp_test::VerifyIssue713GratingAmplitude(RunVulkanLensPass);
}

TEST_F(VulkanLensCalibTest, HighlightEdgeHasNoRingingAndMatchesCpuReference) {
  lens_warp_test::VerifyHighlightEdgeHasNoRinging(RunVulkanLensPass);
}

TEST_F(VulkanLensCalibTest, VignettingOnlyDoesNotResample) {
  lens_warp_test::VerifyVignettingOnlyDoesNotResample(RunVulkanLensPass);
}

// ---- Against OpenCL --------------------------------------------------------------------------

TEST_F(VulkanLensCalibTest, EachProjectionEqualsOpenCl) {
  // The kernel converts the target projection to erect and erect to the source projection, so
  // the pairs below run each conversion of both directions once.
  struct Pair {
    LensCalibProjectionType source;
    LensCalibProjectionType target;
  };
  using P                       = LensCalibProjectionType;
  const std::vector<Pair> pairs = {
      {P::RECTILINEAR, P::FISHEYE},
      {P::RECTILINEAR, P::PANORAMIC},
      {P::RECTILINEAR, P::EQUIRECTANGULAR},
      {P::RECTILINEAR, P::FISHEYE_ORTHOGRAPHIC},
      {P::RECTILINEAR, P::FISHEYE_STEREOGRAPHIC},
      {P::RECTILINEAR, P::FISHEYE_EQUISOLID},
      {P::RECTILINEAR, P::FISHEYE_THOBY},
      {P::FISHEYE, P::RECTILINEAR},
      {P::PANORAMIC, P::RECTILINEAR},
      {P::FISHEYE_ORTHOGRAPHIC, P::RECTILINEAR},
      {P::FISHEYE_STEREOGRAPHIC, P::RECTILINEAR},
      {P::FISHEYE_EQUISOLID, P::RECTILINEAR},
      {P::FISHEYE_THOBY, P::RECTILINEAR},
  };
  const cv::Mat source = PositionRamp(lens_warp_test::kTestWidth, lens_warp_test::kTestHeight);
  for (const auto& pair : pairs) {
    LensCalibGpuParams params = Issue713Params();
    params.apply_projection   = 1;
    params.source_projection  = static_cast<std::int32_t>(pair.source);
    params.target_projection  = static_cast<std::int32_t>(pair.target);
    ExpectEqualToOpenCl("projection " + std::to_string(params.source_projection) + " from " +
                            std::to_string(params.target_projection),
                        source, params);
  }
}

TEST_F(VulkanLensCalibTest, RectangleCropEqualsOpenCl) {
  const cv::Mat      source = PositionRamp(lens_warp_test::kTestWidth, lens_warp_test::kTestHeight);
  LensCalibGpuParams params = Issue713Params();
  params.apply_crop         = 1;
  params.crop_mode          = static_cast<std::int32_t>(LensCalibCropMode::RECTANGLE);
  params.crop_bounds[0]     = 0.08F;
  params.crop_bounds[1]     = 0.93F;
  params.crop_bounds[2]     = 0.05F;
  params.crop_bounds[3]     = 0.9F;
  const auto steps = vulkan::lens::ResolveLensCalibrationSteps(params, lens_warp_test::kTestWidth,
                                                               lens_warp_test::kTestHeight);
  ASSERT_TRUE(steps.warp);
  ASSERT_TRUE(steps.rectangle_crop.has_value());
  EXPECT_EQ(steps.rectangle_crop->x, 51);
  EXPECT_EQ(steps.rectangle_crop->width, 544);
  ExpectEqualToOpenCl("rectangle crop after the warp", source, params);

  // The crop without a warp: vignetting, crop, resize.
  params.apply_distortion    = 0;
  params.apply_tca           = 0;
  params.apply_vignetting    = 1;
  params.vignetting_terms[0] = -0.18F;
  params.vignetting_terms[1] = 0.01F;
  params.vignetting_terms[2] = -0.07F;
  ExpectEqualToOpenCl("rectangle crop after the vignetting", source, params);
}

TEST_F(VulkanLensCalibTest, CircleCropEqualsOpenCl) {
  const cv::Mat      source = PositionRamp(lens_warp_test::kTestWidth, lens_warp_test::kTestHeight);
  LensCalibGpuParams params = Issue713Params();
  params.apply_crop         = 1;
  params.apply_crop_circle  = 1;
  params.crop_mode          = static_cast<std::int32_t>(LensCalibCropMode::CIRCLE);
  params.crop_bounds[0]     = 0.1F;
  params.crop_bounds[1]     = 0.9F;
  params.crop_bounds[2]     = 0.0F;
  params.crop_bounds[3]     = 1.0F;
  const cv::Mat vulkan      = RunVulkanLensPass(source, params);
  // Outside the circle alpha is 0; at the center it is 1.
  EXPECT_EQ(vulkan.at<cv::Vec4f>(4, 4)[3], 0.0F);
  EXPECT_EQ(
      vulkan.at<cv::Vec4f>(lens_warp_test::kTestHeight / 2, lens_warp_test::kTestWidth / 2)[3],
      1.0F);
  ExpectEqualToOpenCl("circle crop", source, params);
}

TEST_F(VulkanLensCalibTest, StepsWithoutAnEffectRecordNothing) {
  LensCalibGpuParams params = Issue713Params();
  params.apply_distortion   = 0;
  params.apply_tca          = 0;
  params.apply_vignetting   = 0;
  params.apply_crop         = 1;
  params.crop_mode          = static_cast<std::int32_t>(LensCalibCropMode::RECTANGLE);
  params.crop_bounds[0]     = 0.0F;  // the full image: no crop step
  params.crop_bounds[1]     = 1.0F;
  params.crop_bounds[2]     = 0.0F;
  params.crop_bounds[3]     = 1.0F;
  const auto steps          = vulkan::lens::ResolveLensCalibrationSteps(params, 64, 48);
  EXPECT_FALSE(steps.Any());

  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  auto&              context = device.CommandContext();
  params.src_width           = 64;
  params.src_height          = 48;
  params.dst_width           = 64;
  params.dst_height          = 48;
  auto image                 = backend.CreateTexture2D(64, 48, TextureFormat::Rgba32f);
  auto parameters            = backend.CreateBuffer(vulkan::lens::kLensCalibParameterBytes);
  backend.ResetCounters();
  const auto before = vulkan::ReadVulkanApiCounters();
  vulkan::lens::EncodeLensCalibration(backend, context, image, {}, VulkanBufferView{&parameters, 0},
                                      params);
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
  EXPECT_FALSE(context.IsRecording());

  // Vignetting alone: one upload and one dispatch, recorded without a submission.
  params.apply_vignetting      = 1;
  params.vignetting_terms[0]   = -0.2F;
  const auto before_vignetting = vulkan::ReadVulkanApiCounters();
  vulkan::lens::EncodeLensCalibration(backend, context, image, {}, VulkanBufferView{&parameters, 0},
                                      params);
  const auto vignetting =
      vulkan::VulkanApiCounterDelta(before_vignetting, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(vignetting.dispatches, 1U);
  EXPECT_EQ(vignetting.submits, 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 1U);
  EXPECT_EQ(backend.HostToDeviceBytes(), vulkan::lens::kLensCalibParameterBytes);
  EXPECT_TRUE(context.IsRecording());
  backend.Wait(context);
}

TEST_F(VulkanLensCalibTest, InvalidRequestIsRejectedBeforeACommand) {
  constexpr std::uint32_t kWidth = 64, kHeight = 48;
  VulkanRenderDevice      device;
  auto&                   backend = device.Workspace().Device();
  auto&                   context = device.CommandContext();
  auto                    image  = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto                    warped = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::Rgba32f);
  auto one_row_short      = backend.CreateTexture2D(kWidth, kHeight - 1, TextureFormat::Rgba32f);
  auto r32f               = backend.CreateTexture2D(kWidth, kHeight, TextureFormat::R32f);
  auto parameters         = backend.CreateBuffer(vulkan::lens::kLensCalibParameterBytes);

  LensCalibGpuParams warp = lens_warp_test::ResolveIssue713Params(kWidth, kHeight);
  LensCalibGpuParams crop = warp;
  crop.apply_crop         = 1;
  crop.crop_mode          = static_cast<std::int32_t>(LensCalibCropMode::RECTANGLE);
  crop.crop_bounds[0]     = 0.1F;
  crop.crop_bounds[1]     = 0.9F;
  crop.crop_bounds[2]     = 0.1F;
  crop.crop_bounds[3]     = 0.9F;
  LensCalibGpuParams other_size = warp;
  other_size.dst_width          = kWidth * 2;
  const VulkanBufferView view{&parameters, 0};

  struct Rejected {
    const char*           name;
    std::function<void()> encode;
  };
  const std::vector<Rejected> requests = {
      {"warp without a warp texture",
       [&] { vulkan::lens::EncodeLensCalibration(backend, context, image, {}, view, warp); }},
      {"warp texture of another size",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, image, {&one_row_short, nullptr},
                                             view, warp);
       }},
      {"warp texture that is the image",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, image, {&image, nullptr}, view,
                                             warp);
       }},
      {"rectangle crop without a crop texture",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, image, {&warped, nullptr}, view,
                                             crop);
       }},
      {"crop texture of the image size",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, image, {&warped, &one_row_short},
                                             view, crop);
       }},
      {"parameters for another size",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, image, {&warped, nullptr}, view,
                                             other_size);
       }},
      {"parameter view past the end",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, image, {&warped, nullptr},
                                             VulkanBufferView{&parameters, 16}, warp);
       }},
      {"R32F image",
       [&] {
         vulkan::lens::EncodeLensCalibration(backend, context, r32f, {&warped, nullptr}, view,
                                             warp);
       }},
  };

  backend.ResetCounters();
  const auto before = vulkan::ReadVulkanApiCounters();
  for (const auto& request : requests) {
    EXPECT_THROW(request.encode(), std::invalid_argument) << request.name;
  }
  const auto delta = vulkan::VulkanApiCounterDelta(before, vulkan::ReadVulkanApiCounters());
  EXPECT_EQ(delta.dispatches, 0U);
  EXPECT_EQ(delta.pipeline_creations + delta.pipeline_cache_hits, 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
  EXPECT_FALSE(context.IsRecording());
}

}  // namespace
}  // namespace alcedo
