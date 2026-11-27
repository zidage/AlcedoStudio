//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The geometry resample, the camera color, and the raster input passes of the Vulkan backend
// (#1036, phase VK7): against the host evaluation of the shared per-pixel code, OpenColorIO, and
// the same passes of the reference backend on the same GPU (decision B9).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "aces2_ocio_reference.hpp"
#include "dng_profile_test_support.hpp"
#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/display_to_ap1_math.h"
#include "edit/runtime/drt/aces2_reference_runtime.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/pass_kind.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_develop_pass.hpp"
#include "edit/runtime/vulkan/vulkan_display_to_ap1_pass.hpp"
#include "edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp"
#include "edit/runtime/vulkan/vulkan_pass_encoder.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "image/raster_color_description.hpp"
#include "raster_render_test_support.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

namespace alcedo {
namespace {

using namespace vulkan_reference;

/// The CI Sony ARW of the DCP test of the other backends (VerifyDcpCameraProfile).
constexpr const char* kCiArw =
    "Tag @ryanbreitkreutz - Free files from @signatureeditscoDSC00830.ARW";

/// The images of one render: `geometry.scene_source` and `develop.image`.
struct DevelopImages {
  std::vector<float> geometry;
  std::vector<float> develop;
  std::uint32_t      width = 0;
};

/// Develop, GeometryResample, then @p color (CameraToAp1 or DisplayToAp1) on a new device of
/// @p Device. With @p geometry_values, `geometry.scene_source` is replaced by those values before
/// @p color, so the color pass of two backends reads the same input.
template <class Device, class Develop, class Geometry, class Color>
auto RenderDevelopImage(const ExecutionPlan& plan, const PreparedRawInput& input,
                        const PipelineDocument& document, Develop develop, Geometry geometry,
                        Color color, const std::vector<float>* geometry_values = nullptr)
    -> DevelopImages {
  Device device;
  device.BeginRender();
  develop(device, plan, input, document);
  geometry(device, plan);
  if (geometry_values != nullptr) {
    auto* lease = device.Workspace().Images().Find(plan.geometry_output);
    EXPECT_NE(lease, nullptr);
    if (lease != nullptr) {
      device.Workspace().Device().UploadTexture2D(
          lease->Texture(), std::as_bytes(std::span<const float>(*geometry_values)),
          device.CommandContext());
    }
  }
  color(device, plan, input, document);
  device.EndRender();
  device.WaitIdle();
  DevelopImages images;
  images.geometry = DownloadImage(device, plan.geometry_output);
  images.develop  = DownloadImage(device, plan.develop_output);
  if (const auto* lease = device.Workspace().Images().Find(plan.develop_output)) {
    images.width = lease->Texture().Width();
  }
  return images;
}

void VulkanCamera(VulkanRenderDevice& device, const ExecutionPlan& plan, const PreparedRawInput&,
                  const PipelineDocument& document) {
  ExecuteVulkanCameraColor(device, plan, document);
}

void ReferenceCamera(ReferenceRenderDevice& device, const ExecutionPlan& plan,
                     const PreparedRawInput&, const PipelineDocument&    document) {
  ExecuteReferenceCameraColor(device, plan, document);
}

auto RenderVulkanRaw(const ExecutionPlan& plan, const PreparedRawInput& input,
                     const PipelineDocument&   document,
                     const std::vector<float>* geometry_values = nullptr) -> DevelopImages {
  return RenderDevelopImage<VulkanRenderDevice>(plan, input, document, ExecuteVulkanDevelop,
                                                ExecuteVulkanGeometryResample, VulkanCamera,
                                                geometry_values);
}

auto RenderReferenceRaw(const ExecutionPlan& plan, const PreparedRawInput& input,
                        const PipelineDocument&   document,
                        const std::vector<float>* geometry_values = nullptr) -> DevelopImages {
  return RenderDevelopImage<ReferenceRenderDevice>(plan, input, document, ExecuteReferenceDevelop,
                                                   ExecuteReferenceGeometryResample,
                                                   ReferenceCamera, geometry_values);
}

auto RenderVulkanRaster(const ExecutionPlan& plan, const PreparedRawInput& input,
                        const PipelineDocument& document) -> DevelopImages {
  return RenderDevelopImage<VulkanRenderDevice>(plan, input, document, ExecuteVulkanDevelop,
                                                ExecuteVulkanGeometryResample,
                                                ExecuteVulkanDisplayToAp1);
}

auto RenderReferenceRaster(const ExecutionPlan& plan, const PreparedRawInput& input,
                           const PipelineDocument& document) -> DevelopImages {
  return RenderDevelopImage<ReferenceRenderDevice>(plan, input, document, ExecuteReferenceDevelop,
                                                   ExecuteReferenceGeometryResample,
                                                   ExecuteReferenceDisplayToAp1);
}

/// The CI ARW at full size and its color metadata, decoded one time for the run.
struct CameraRawCase {
  std::shared_ptr<const PreparedRawInput> input;
  RawRuntimeColorContext                  color;
};

auto LoadCiArw() -> std::shared_ptr<const CameraRawCase> {
  static std::shared_ptr<const CameraRawCase> loaded;
  if (loaded) {
    return loaded;
  }
  const auto    path = std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles" / kCiArw;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return nullptr;
  }
  std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Image image(1, path, ImageType::ARW);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  if (!image.HasRawColorContext()) {
    return nullptr;
  }
  auto result = std::make_shared<CameraRawCase>();
  result->input =
      std::make_shared<const PreparedRawInput>(RawInputLoader::LoadEncoded(bytes, DecodeRes::FULL));
  result->color = image.GetRawColorContext();
  loaded        = result;
  return loaded;
}

/// The document of @p raw: its imported camera profile, the legacy demosaic, and no highlight
/// reconstruction (the DCP test of the other backends).
auto MakeCameraDocument(const CameraRawCase& raw) -> PipelineDocument {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::ApplyImportedCameraProfile(document, raw.color);
  auto payload                   = document.Develop()->Params().Params();
  payload.demosaic_method        = "legacy";
  payload.highlights_reconstruct = false;
  document.Develop()->Params().ReplaceParams(payload);
  return document;
}

/// The texel center (x + 0.5, y + 0.5) mapped by the 2 x 3 rows of @p m.
struct Point {
  float x = 0.0F;
  float y = 0.0F;
};

auto RenderToDecoded(const float* m, std::uint32_t x, std::uint32_t y) -> Point {
  const float cx = static_cast<float>(x) + 0.5F;
  const float cy = static_cast<float>(y) + 0.5F;
  return {m[0] * cx + m[1] * cy + m[2], m[3] * cx + m[4] * cy + m[5]};
}

/// The host form of the bilinear and bicubic samples of geometry_camera.cl.
struct HostImage {
  std::vector<float> values;
  int                width  = 0;
  int                height = 0;

  [[nodiscard]] auto Read(int x, int y, const float* border, int c) const -> float {
    if (x < 0 || y < 0 || x >= width || y >= height) {
      return border[c];
    }
    return values[(static_cast<std::size_t>(y) * width + x) * 4 + c];
  }

  [[nodiscard]] auto Bilinear(float sx, float sy, const float* border, int c) const -> float {
    const float px = sx - 0.5F;
    const float py = sy - 0.5F;
    const int   x0 = static_cast<int>(std::floor(px));
    const int   y0 = static_cast<int>(std::floor(py));
    const float fx = px - static_cast<float>(x0);
    const float fy = py - static_cast<float>(y0);
    return Read(x0, y0, border, c) * (1.0F - fx) * (1.0F - fy) +
           Read(x0 + 1, y0, border, c) * fx * (1.0F - fy) +
           Read(x0, y0 + 1, border, c) * (1.0F - fx) * fy +
           Read(x0 + 1, y0 + 1, border, c) * fx * fy;
  }

  static auto CubicWeight(float x) -> float {
    x = std::fabs(x);
    if (x < 1.0F) {
      return ((1.5F * x - 2.5F) * x) * x + 1.0F;
    }
    if (x < 2.0F) {
      return (((-0.5F * x + 2.5F) * x) - 4.0F) * x + 2.0F;
    }
    return 0.0F;
  }

  [[nodiscard]] auto Bicubic(float sx, float sy, const float* border, int c) const -> float {
    const float px   = sx - 0.5F;
    const float py   = sy - 0.5F;
    const int   x0   = static_cast<int>(std::floor(px));
    const int   y0   = static_cast<int>(std::floor(py));
    const float fx   = px - static_cast<float>(x0);
    const float fy   = py - static_cast<float>(y0);
    float       acc  = 0.0F;
    float       wsum = 0.0F;
    for (int j = -1; j <= 2; ++j) {
      const float wy = CubicWeight(static_cast<float>(j) - fy);
      for (int i = -1; i <= 2; ++i) {
        const float w = CubicWeight(static_cast<float>(i) - fx) * wy;
        acc += Read(x0 + i, y0 + j, border, c) * w;
        wsum += w;
      }
    }
    return wsum <= 1.0e-8F ? border[c] : acc / wsum;
  }
};

/// A position ramp in R and G and a texture of 17 levels in B: the bicubic weights see detail.
auto MakeGeometrySource(int width, int height) -> HostImage {
  HostImage image;
  image.width  = width;
  image.height = height;
  image.values.resize(static_cast<std::size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float* p = &image.values[(static_cast<std::size_t>(y) * width + x) * 4];
      p[0]     = (static_cast<float>(x) + 0.5F) / static_cast<float>(width);
      p[1]     = (static_cast<float>(y) + 0.5F) / static_cast<float>(height);
      p[2]     = static_cast<float>((x * 7 + y * 13) % 17) / 16.0F;
      p[3]     = 1.0F;
    }
  }
  return image;
}

/// Develop of a 64 x 48 DirectRgb input, then the source values of @p source in
/// `develop.sensor_linear`, then GeometryResample (the OpenCL geometry test).
template <class Device, class Develop, class Geometry>
auto RenderGeometry(const ExecutionPlan& plan, const PreparedRawInput& input,
                    const PipelineDocument& document, const HostImage& source, Develop develop,
                    Geometry geometry) -> std::vector<float> {
  Device device;
  device.BeginRender();
  develop(device, plan, input, document);
  auto* sensor = device.Workspace().Images().Find(plan.sensor_linear_output);
  EXPECT_NE(sensor, nullptr);
  if (sensor == nullptr) {
    device.CancelRender();
    return {};
  }
  device.Workspace().Device().UploadTexture2D(sensor->Texture(),
                                              std::as_bytes(std::span<const float>(source.values)),
                                              device.CommandContext());
  geometry(device, plan);
  device.EndRender();
  device.WaitIdle();
  return DownloadImage(device, plan.geometry_output);
}

/// The DisplayToAp1 kernel of @p output_kind over interleaved RGB (rows of at most 1024 texels,
/// alpha 0.25); returns interleaved RGB and expects alpha unchanged.
auto RunDisplayToAp1(std::span<const float> packed, const std::vector<float>& rgb,
                     DisplayToAp1Output output_kind) -> std::vector<float> {
  const std::size_t  pixels = rgb.size() / 3;
  const auto         width  = static_cast<std::uint32_t>(std::min<std::size_t>(pixels, 1024));
  const auto         height = static_cast<std::uint32_t>((pixels + width - 1) / width);
  std::vector<float> host(static_cast<std::size_t>(width) * height * 4, 0.0F);
  for (std::size_t i = 0; i < pixels; ++i) {
    host[i * 4]     = rgb[i * 3];
    host[i * 4 + 1] = rgb[i * 3 + 1];
    host[i * 4 + 2] = rgb[i * 3 + 2];
    host[i * 4 + 3] = 0.25F;
  }
  VulkanRenderDevice device;
  auto&              backend     = device.Workspace().Device();
  auto&              context     = device.CommandContext();
  auto               source      = backend.CreateTexture2D(width, height, TextureFormat::Rgba32f);
  auto               destination = backend.CreateTexture2D(width, height, TextureFormat::Rgba32f);
  auto               parameters  = backend.CreateBuffer(packed.size_bytes());
  backend.UploadTexture2D(source, std::as_bytes(std::span<const float>(host)), context);
  backend.UploadBufferRange(parameters, 0, std::as_bytes(packed), context);
  EncodeVulkanDisplayToAp1(backend, context, source, destination, parameters, 0, packed.size(),
                           output_kind);
  backend.SynchronizeRecordedWork(context);
  backend.DownloadTexture2D(destination, std::as_writable_bytes(std::span<float>(host)), context);
  std::vector<float> out(rgb.size());
  for (std::size_t i = 0; i < pixels; ++i) {
    out[i * 3]     = host[i * 4];
    out[i * 3 + 1] = host[i * 4 + 1];
    out[i * 3 + 2] = host[i * 4 + 2];
    EXPECT_EQ(host[i * 4 + 3], 0.25F);
  }
  return out;
}

auto AsRgba(const std::vector<float>& values) -> std::vector<raster_render_test::Rgba> {
  std::vector<raster_render_test::Rgba> pixels(values.size() / 4);
  std::memcpy(pixels.data(), values.data(), pixels.size() * sizeof(raster_render_test::Rgba));
  return pixels;
}

class VulkanGeometryColorTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
  }
};

// The tolerances of the measurement of this phase (plan, VK7 completion record).
/// The host sample against a GPU sample (the OpenCL geometry test).
constexpr float     kHostGeometryTolerance = 1.5e-4F;
constexpr Tolerance kGeometryTolerance     = Uniform(1.0e-5);
/// The camera pass of the two backends on the same `geometry.scene_source`: the root mean square
/// of the differences. A value that differs by more than kCameraValueTolerance must equal the host
/// evaluation within kCameraHostTolerance (the tolerance of ExpectCameraPassMatchesScalarProfile).
/// On the Apple M4, 3 of 56 million values differ by up to 0.039 in ACEScc near black, where Metal
/// (fast math) leaves the host value and Vulkan is equal to it (VK7 record).
constexpr Tolerance kCameraTolerance       = {INFINITY, INFINITY, 0, 1.0e-5};
constexpr float     kCameraValueTolerance  = 1.0e-4F;
constexpr float     kCameraHostTolerance   = 3.0e-4F;
#if defined(HAVE_OPENCL)
constexpr Tolerance kRawDevelopTolerance = Uniform(1.0e-4);
#else
/// The Metal RAW kernels have fast math and contract products into FMAs (VK6 record); the
/// demosaic difference reaches `develop.image` through the camera pass.
constexpr Tolerance kRawDevelopTolerance = {0.5, 0.5, 0, 2.5e-4};
#endif

TEST_F(VulkanGeometryColorTest, GeometryResampleEqualsTheHostSampleAndTheReferenceBackend) {
  ImageGeometryParams image;
  image.crop_rect        = NormalizedRect{0.25F, 0.25F, 0.50F, 0.50F};
  image.rotation_degrees = 15.0F;
  ViewRequest view;
  view.visible_rect_in_edit_space = NormalizedRect{0.10F, 0.10F, 0.80F, 0.80F};
  view.viewport_extent            = Extent2D{40, 30};
  const auto source_geometry      = MakeSourceGeometry({64, 48}, {64, 48});
  auto       geometry             = ResolveRenderGeometry(source_geometry, image, view, {}, {});
  ASSERT_NE(geometry.render_extent, geometry.decoded_extent);

  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto    input  = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                       gpu_dag_test::FullSensor(64, 48));
  const auto    source = MakeGeometrySource(64, 48);
  RenderRequest request;
  request.view = view;
  for (const auto filter : {TextureFilter::Bilinear, TextureFilter::Bicubic}) {
    const bool bicubic = filter == TextureFilter::Bicubic;
    SCOPED_TRACE(bicubic ? "bicubic" : "bilinear");
    geometry.filter = filter;
    auto plan       = GraphCompiler::Compile(document, input.CompileSource(), request);
    plan.geometry   = geometry;
    plan.encode_geometry_resample = true;

    const auto vulkan             = RenderGeometry<VulkanRenderDevice>(
        plan, input, document, source, ExecuteVulkanDevelop, ExecuteVulkanGeometryResample);
    const auto reference = RenderGeometry<ReferenceRenderDevice>(
        plan, input, document, source, ExecuteReferenceDevelop, ExecuteReferenceGeometryResample);
    const auto render_w = geometry.render_extent.width;
    const auto render_h = geometry.render_extent.height;
    ASSERT_EQ(vulkan.size(), static_cast<std::size_t>(render_w) * render_h * 4);
    float largest = 0.0F;
    for (std::uint32_t y = 0; y < render_h; ++y) {
      for (std::uint32_t x = 0; x < render_w; ++x) {
        const auto  at  = RenderToDecoded(geometry.gpu_data.render_to_decoded, x, y);
        const auto* gpu = &vulkan[(static_cast<std::size_t>(y) * render_w + x) * 4];
        for (int c = 0; c < 4; ++c) {
          const float host = bicubic
                                 ? source.Bicubic(at.x, at.y, geometry.gpu_data.border_rgba, c)
                                 : source.Bilinear(at.x, at.y, geometry.gpu_data.border_rgba, c);
          largest          = std::max(largest, std::fabs(host - gpu[c]));
        }
      }
    }
    std::printf("geometry %s: host against Vulkan, largest %.3g\n",
                bicubic ? "bicubic" : "bilinear", static_cast<double>(largest));
    EXPECT_LT(largest, kHostGeometryTolerance);
    ExpectEqualToReference(std::string("geometry ") + (bicubic ? "bicubic" : "bilinear"), reference,
                           vulkan, render_w, kGeometryTolerance);
  }
}

TEST_F(VulkanGeometryColorTest, GeometryWithoutResampleAliasesTheSensorImage) {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                   gpu_dag_test::FullSensor(64, 48));
  const auto plan  = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  ASSERT_FALSE(plan.encode_geometry_resample);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  device.BeginRender();
  ExecuteVulkanDevelop(device, plan, input, document);
  const auto textures = backend.TextureCreateCount();
  ExecuteVulkanGeometryResample(device, plan);
  EXPECT_EQ(backend.TextureCreateCount(), textures);
  const auto* sensor   = device.Workspace().Images().Find(plan.sensor_linear_output);
  const auto* geometry = device.Workspace().Images().Find(plan.geometry_output);
  ASSERT_NE(sensor, nullptr);
  ASSERT_NE(geometry, nullptr);
  EXPECT_EQ(geometry->Texture().ResourceId(), sensor->Texture().ResourceId());
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(DownloadImage(device, plan.geometry_output),
            DownloadImage(device, plan.sensor_linear_output));
}

TEST_F(VulkanGeometryColorTest, CameraColorWithADcpProfileMatchesTheScalarProfile) {
  const auto loaded = LoadCiArw();
  ASSERT_NE(loaded, nullptr) << "CI RAW fixture is missing or has no color metadata: " << kCiArw;
  auto       document = MakeCameraDocument(*loaded);
  // The synthetic DCP of the other backends: two hue and saturation maps and a look table.
  const auto dcp_path = std::filesystem::temp_directory_path() / "alcedo_vk7_dcp_vulkan.dcp";
  gpu_dag_test::SyntheticDcpProfileFile().Write(dcp_path);
  const auto      dcp = ReadDcpCameraProfile(dcp_path);
  std::error_code removed;
  std::filesystem::remove(dcp_path, removed);
  ASSERT_TRUE(dcp && dcp->tables);
  DevelopCameraProfileFileUpdate update;
  update.profile = dcp;
  document.Develop()->Params().ApplyCameraProfileFileUpdate(std::move(update));
  const auto payload = document.Develop()->Params().Params();
  const auto plan =
      GraphCompiler::Compile(document, loaded->input->CompileSource(), RenderRequest{});

  {
    VulkanRenderDevice device;
    device.BeginRender();
    ExecuteVulkanDevelop(device, plan, *loaded->input, document);
    ExecuteVulkanGeometryResample(device, plan);
    ExecuteVulkanCameraColor(device, plan, document);
    device.EndRender();
    device.WaitIdle();
    gpu_dag_test::ExpectCameraPassMatchesScalarProfile(device, plan, payload, "vulkan");
  }

  // The camera pass of the reference backend on the `geometry.scene_source` of Vulkan.
  const auto vulkan    = RenderVulkanRaw(plan, *loaded->input, document);
  const auto reference = RenderReferenceRaw(plan, *loaded->input, document, &vulkan.geometry);
  ExpectEqualToReference("camera color with a DCP profile", reference.develop, vulkan.develop,
                         vulkan.width, kCameraTolerance);
  // Where the backends differ: the host evaluation (the scalar form of the kernel) decides.
  const auto transform = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(transform.ok);
  const auto  table     = PackDngProfileGpuData(payload, transform.transform);
  std::size_t differing = 0;
  for (std::size_t i = 0; i < vulkan.develop.size(); ++i) {
    if (i % 4 == 3 || std::abs(vulkan.develop[i] - reference.develop[i]) <= kCameraValueTolerance) {
      continue;
    }
    const float* s    = &vulkan.geometry[i / 4 * 4];
    float        c[3] = {};
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 3; ++k) {
        c[r] += transform.transform.camera_to_ap1[r * 3 + k] * s[k];
      }
    }
    const auto  corrected  = DngApplyColorProfile(DngMakeRgb(c[0], c[1], c[2]), table.data());
    const auto  compressed = AcesReferenceGamutCompress(corrected.r, corrected.g, corrected.b);
    const float host[3]    = {gpu_dag_test::EncodeAcesCc(compressed.r),
                              gpu_dag_test::EncodeAcesCc(compressed.g),
                              gpu_dag_test::EncodeAcesCc(compressed.b)};
    const float expected   = host[i % 4];
    if (++differing <= 8) {
      std::printf("  texel (%zu, %zu) channel %zu: host %.9g, Vulkan %.9g, %s %.9g\n",
                  i / 4 % vulkan.width, i / 4 / vulkan.width, i % 4, static_cast<double>(expected),
                  static_cast<double>(vulkan.develop[i]), kReferenceBackend,
                  static_cast<double>(reference.develop[i]));
    }
    EXPECT_NEAR(vulkan.develop[i], expected, kCameraHostTolerance)
        << "texel " << i / 4 % vulkan.width << ", " << i / 4 / vulkan.width;
  }
  std::printf("camera color: %zu values differ by more than %.3g\n", differing,
              static_cast<double>(kCameraValueTolerance));
}

TEST_F(VulkanGeometryColorTest, RawDevelopImageEqualsTheReferenceBackend) {
  const auto loaded = LoadCiArw();
  ASSERT_NE(loaded, nullptr) << "CI RAW fixture is missing or has no color metadata: " << kCiArw;
  const auto document = MakeCameraDocument(*loaded);
  const auto plan =
      GraphCompiler::Compile(document, loaded->input->CompileSource(), RenderRequest{});
  ASSERT_TRUE(plan.Contains(GpuPassKind::CameraToAp1));
  const auto vulkan    = RenderVulkanRaw(plan, *loaded->input, document);
  const auto reference = RenderReferenceRaw(plan, *loaded->input, document);
  ExpectEqualToReference("ARW develop.image", reference.develop, vulkan.develop, vulkan.width,
                         kRawDevelopTolerance);
}

TEST_F(VulkanGeometryColorTest, RasterDevelopOutputMatchesTheHostEvaluationForEachRasterKind) {
  struct Case {
    const char*    name;
    RasterFileKind kind;
  };
  for (const auto& entry : {Case{"srgb_chunk.png", RasterFileKind::Png},
                            Case{"cicp_rec2020_pq_16bit.png", RasterFileKind::Png},
                            Case{"prophoto_icc_16bit.tif", RasterFileKind::Tiff},
                            Case{"lut_based_rgb_icc.tif", RasterFileKind::Tiff},
                            Case{"lut_icc_with_cicp_rec2020_pq_16bit.tif", RasterFileKind::Tiff},
                            Case{"chromaticities_p3_half.exr", RasterFileKind::OpenExr},
                            Case{"alcedo_export_rec2020_hlg.tif", RasterFileKind::Tiff},
                            Case{"alcedo_export_upstream_rec2020_v4.tif", RasterFileKind::Tiff}}) {
    SCOPED_TRACE(entry.name);
    const auto c    = raster_render_test::LoadCase(entry.name, entry.kind);
    const auto plan = GraphCompiler::Compile(c.document, c.input.CompileSource(), RenderRequest{});
    ASSERT_TRUE(plan.Contains(GpuPassKind::DisplayToAp1));
    ASSERT_FALSE(plan.Contains(GpuPassKind::CameraToAp1));
    const auto vulkan    = RenderVulkanRaster(plan, c.input, c.document);
    const auto reference = RenderReferenceRaster(plan, c.input, c.document);
    raster_render_test::ExpectAcesccNear(
        AsRgba(vulkan.develop),
        raster_render_test::HostReferenceDevelopOutput(c.input, c.description), entry.name);
    raster_render_test::ExpectAcesccNear(
        AsRgba(vulkan.develop), AsRgba(reference.develop),
        (std::string(entry.name) + " against " + kReferenceBackend).c_str());
    // The report of the difference; ExpectAcesccNear above is the check.
    ExpectEqualToReference(entry.name, reference.develop, vulkan.develop, vulkan.width,
                           Uniform(INFINITY));
  }
}

TEST_F(VulkanGeometryColorTest, Aces2InverseMatchesOcioCpuProcessor) {
  for (const auto& c : aces2_ocio_reference::Aces2Cases()) {
    SCOPED_TRACE(c.name_);
    const auto runtime = ResolveAces2ReferenceRuntime(c.primaries_, c.peak_nits_);
    const auto grid    = aces2_ocio_reference::DisplayGrid(c.peak_nits_);
    aces2_ocio_reference::ExpectMatchesOcio(
        c, grid, RunDisplayToAp1(runtime->packed_, grid, DisplayToAp1Output::LinearAp0));
  }
}

TEST_F(VulkanGeometryColorTest, AcesccOutputMatchesHostEvaluationForBothBranches) {
  const auto display =
      ResolveAces2ReferenceRuntime(color::GamutPrimariesXy(color::ColorGamutId::P3D65), 100.0F);
  const auto scene = PackSceneLinearToAp1(color::GamutPrimariesXy(color::ColorGamutId::Ap0));
  std::vector<float> rgb;
  for (int i = 0; i < 64; ++i) {
    rgb.push_back(static_cast<float>(i % 4) / 3.0F);
    rgb.push_back(static_cast<float>((i / 4) % 4) / 3.0F);
    rgb.push_back(static_cast<float>(i / 16) / 3.0F);
  }
  for (const std::span<const float> packed :
       {std::span<const float>(display->packed_), std::span<const float>(scene)}) {
    const auto gpu = RunDisplayToAp1(packed, rgb, DisplayToAp1Output::AcesccAp1);
    // The tolerance of the OpenCL test: 2e-4 in ACEScc is 0.25 percent in linear light.
    for (std::size_t p = 0; p < rgb.size() / 3; ++p) {
      const auto host =
          D2aSourceToAcesccAp1(A2rMake3(rgb[p * 3], rgb[p * 3 + 1], rgb[p * 3 + 2]), packed.data());
      EXPECT_NEAR(gpu[p * 3], host.x, 2e-4F) << "pixel " << p;
      EXPECT_NEAR(gpu[p * 3 + 1], host.y, 2e-4F) << "pixel " << p;
      EXPECT_NEAR(gpu[p * 3 + 2], host.z, 2e-4F) << "pixel " << p;
    }
  }
}

TEST_F(VulkanGeometryColorTest, PassesRecordIntoTheOpenRecordingWithoutASubmission) {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto    input = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                      gpu_dag_test::FullSensor(64, 48));
  RenderRequest request;
  request.resolution.render_scale = 0.5F;
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), request);
  ASSERT_TRUE(plan.encode_geometry_resample);
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  backend.ResetCounters();
  device.BeginRender();
  PassEncoder<VulkanBackend, GpuPassKind::UploadRgb>::Encode(device, plan, input, document);
  PassEncoder<VulkanBackend, GpuPassKind::GeometryResample>::Encode(device, plan, input, document);
  PassEncoder<VulkanBackend, GpuPassKind::CameraToAp1>::Encode(device, plan, input, document);
  EXPECT_EQ(backend.SubmitCount(), 0U);
  EXPECT_EQ(backend.WaitCount(), 0U);
  EXPECT_TRUE(device.CommandContext().IsRecording());
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(backend.SubmitCount(), 1U);
  const auto pixels = DownloadImage(device, plan.develop_output);
  ASSERT_EQ(pixels.size(), static_cast<std::size_t>(plan.geometry.render_extent.width) *
                               plan.geometry.render_extent.height * 4U);
  EXPECT_TRUE(std::all_of(pixels.begin(), pixels.end(), [](float v) { return std::isfinite(v); }));
}

TEST_F(VulkanGeometryColorTest, InvalidRequestsFailBeforeACommand) {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto input  = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                    gpu_dag_test::FullSensor(64, 48));
  const auto plan   = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  const auto raster = raster_render_test::LoadCase("srgb_chunk.png", RasterFileKind::Png);
  const auto raster_plan =
      GraphCompiler::Compile(raster.document, raster.input.CompileSource(), RenderRequest{});

  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  backend.ResetCounters();
  // Outside BeginRender.
  EXPECT_THROW(ExecuteVulkanGeometryResample(device, plan), std::runtime_error);
  EXPECT_THROW(ExecuteVulkanCameraColor(device, plan, document), std::runtime_error);
  EXPECT_THROW(ExecuteVulkanDisplayToAp1(device, raster_plan, raster.input, raster.document),
               std::runtime_error);

  device.BeginRender();
  // No develop.sensor_linear, no geometry.scene_source.
  EXPECT_THROW(ExecuteVulkanGeometryResample(device, plan), std::runtime_error);
  EXPECT_THROW(ExecuteVulkanCameraColor(device, plan, document), std::runtime_error);
  EXPECT_THROW(ExecuteVulkanDisplayToAp1(device, raster_plan, raster.input, raster.document),
               std::runtime_error);
  // A document without a raster input, and one without a Develop node.
  EXPECT_THROW(ExecuteVulkanDisplayToAp1(device, raster_plan, raster.input, document),
               std::runtime_error);
  EXPECT_THROW(ExecuteVulkanCameraColor(device, plan, PipelineDocument{}), std::runtime_error);
  // A raster plane that is not tightly packed.
  auto padded                = raster.input;
  padded.pixels.stride_bytes = padded.pixels.stride_bytes + 4;
  EXPECT_THROW(ExecuteVulkanDevelop(device, raster_plan, padded, raster.document),
               std::runtime_error);
  EXPECT_FALSE(device.CommandContext().IsRecording());
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  EXPECT_EQ(backend.TextureCreateCount(), 0U);
  device.CancelRender();
}

}  // namespace
}  // namespace alcedo
