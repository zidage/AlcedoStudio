//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The neighborhood kernels on the Vulkan backend (#1036, phase VK9): Sharpen, Clarity, and
// Halation through the shared NeighborExecutor with VulkanNeighborOps, against the OpenCL kernels
// on the same GPU (decision B9, Windows) and a host evaluation of primary_grade_neighbor.cl (every
// platform; Metal has no Grade neighborhood kernel). The mix and the mask, the source and
// destination kinds, the steady state, and the failures before a command.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "edit/geometry/render_geometry_resolver.hpp"
#include "edit/geometry/source_geometry.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_neighbor_dispatch.hpp"
#include "vulkan/vulkan_primary_grade_neighbor_kernels.hpp"
#include "vulkan_reference_backend_support.hpp"
#include "vulkan_workspace_test_support.hpp"

#if defined(HAVE_OPENCL)
#include "edit/runtime/opencl/opencl_neighbor_dispatch.hpp"
#include "edit/runtime/opencl/opencl_scene_work.hpp"
#endif

namespace alcedo {
namespace {

using namespace vulkan_reference;

/// The largest relative difference of a value against OpenCL and against the host evaluation.
constexpr double        kNeighborTolerance = 1.0e-5;
/// A Sharpen texel whose high-pass luma is this close to the threshold may take either branch of
/// the threshold test on another device; the comparison leaves it out.
constexpr double        kThresholdMargin   = 1.0e-5;

constexpr std::uint32_t kWidth             = 203;
constexpr std::uint32_t kHeight            = 157;

auto                    Hash(std::uint32_t x, std::uint32_t y, std::uint32_t seed) -> float {
  std::uint32_t h = x * 374761393U + y * 668265263U + seed * 2246822519U;
  h               = (h ^ (h >> 13U)) * 1274126177U;
  h ^= h >> 16U;
  return static_cast<float>(h & 0xffffU) / 65535.0F;
}

/// Display-referred or ACEScc-like values from 0.01 to 0.99: a ramp along x, channel variations
/// along y, a bright square with hard edges, a bright column, and texel noise. The alpha differs
/// between texels, so the kernels must keep it.
auto MakePlane(std::uint32_t width, std::uint32_t height, std::uint32_t seed)
    -> std::vector<float> {
  std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const float base = 0.15F + 0.6F * static_cast<float>(x) / static_cast<float>(width - 1);
      float       r    = base * (1.0F + 0.2F * std::sin(0.31F * static_cast<float>(y)));
      float       g    = base;
      float       b =
          base *
          (1.0F + 0.2F * std::cos(0.23F * static_cast<float>(x) + 0.1F * static_cast<float>(y)));
      if (x >= width / 3 && x < 2 * width / 3 && y >= height / 3 && y < 2 * height / 3) {
        r += 0.3F;
        g += 0.25F;
        b += 0.2F;
      }
      if (x == width / 5) {
        r = g = b = 0.95F;
      }
      const float noise = (Hash(x, y, seed) - 0.5F) * 0.06F;
      const auto  index = (static_cast<std::size_t>(y) * width + x) * 4;
      pixels[index + 0] = std::clamp(r + noise, 0.01F, 0.99F);
      pixels[index + 1] = std::clamp(g + noise * 0.7F, 0.01F, 0.99F);
      pixels[index + 2] = std::clamp(b - noise * 0.5F, 0.01F, 0.99F);
      pixels[index + 3] = 0.25F + 0.25F * static_cast<float>((x + y) % 4);
    }
  }
  return pixels;
}

/// An R8 mask with a ramp along x and a step along y: coverage 0, partial, and 1.
auto MakeMask(std::uint32_t width, std::uint32_t height) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto ramp = static_cast<std::uint32_t>(255U * x / (width - 1));
      bytes[static_cast<std::size_t>(y) * width + x] =
          static_cast<std::uint8_t>(y < height / 4 ? 0U : (y >= 3 * height / 4 ? 255U : ramp));
    }
  }
  return bytes;
}

auto MakeGeometry(std::uint32_t width, std::uint32_t height) -> ResolvedRenderGeometry {
  return ResolveRenderGeometry(MakeSourceGeometry({width, height}, {width, height}), {}, {}, {},
                               {});
}

auto SharpenParams(float amount, float radius, float threshold, std::uint32_t width,
                   std::uint32_t height) -> GradeNeighborParams {
  SharpenModel sharpen;
  sharpen.SetAmount(amount);
  sharpen.SetRadius(radius);
  sharpen.SetThreshold(threshold);
  return MakeGradeNeighborParams(sharpen, AdjustmentBehavior::Sharpen, MakeGeometry(width, height));
}

auto ClarityParams(float value, std::uint32_t width, std::uint32_t height) -> GradeNeighborParams {
  ClarityModel clarity;
  clarity.SetValue(value);
  return MakeGradeNeighborParams(clarity, AdjustmentBehavior::Clarity, MakeGeometry(width, height));
}

auto HalationParams(float value, std::uint32_t width, std::uint32_t height) -> GradeNeighborParams {
  HalationModel halation;
  halation.SetValue(value);
  return MakeGradeNeighborParams(halation, AdjustmentBehavior::Halation,
                                 MakeGeometry(width, height));
}

// ------------------------------------------------------------------------------------------------
// Host evaluation of primary_grade_neighbor.cl (double precision).

struct HostResult {
  std::vector<float> pixels;
  /// One flag for each texel: a Sharpen threshold test within kThresholdMargin.
  std::vector<char>  near_threshold;
};

constexpr std::uint32_t kClarity  = 13;
constexpr std::uint32_t kSharpen  = 14;
constexpr std::uint32_t kHalation = 15;

auto                    HostHalationRadius(float sigma) -> int {
  if (!(sigma > 0.0F)) {
    return 0;
  }
  return std::clamp(static_cast<int>(std::ceil(sigma * 3.0F)), 1,
                                       static_cast<int>(kGradeNeighborMaxTapCount) - 1);
}

/// The Halation window of @p sigma: the weights 1, e^(-1/sigma), ... times the normalization.
auto HostHalationWeights(float sigma) -> std::vector<double> {
  const int           radius = HostHalationRadius(sigma);
  std::vector<double> weights(static_cast<std::size_t>(radius) + 1);
  double              sum = 1.0;
  weights[0]              = 1.0;
  for (int tap = 1; tap <= radius; ++tap) {
    weights[tap] = std::exp(-static_cast<double>(tap) / std::max(sigma, 1.0e-6F));
    sum += 2.0 * weights[tap];
  }
  for (auto& weight : weights) {
    weight /= std::max(sum, 1.0e-6);
  }
  return weights;
}

auto HostLuma(const double* c) -> double { return c[0] * 0.114 + c[1] * 0.587 + c[2] * 0.299; }

auto HostNeighborhood(const std::vector<float>& source, const std::vector<float>& original,
                      const std::vector<std::uint8_t>* mask, float mix,
                      const GradeNeighborParams& p, std::uint32_t width, std::uint32_t height)
    -> HostResult {
  const auto at = [&](int x, int y) {
    x = std::clamp(x, 0, static_cast<int>(width) - 1);
    y = std::clamp(y, 0, static_cast<int>(height) - 1);
    return (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)) * 4;
  };
  const bool          halation = p.behavior == kHalation;
  // Horizontal pass.
  std::vector<double> horizontal(source.size());
  const auto          weights_x = HostHalationWeights(p.sigma_x);
  for (int y = 0; y < static_cast<int>(height); ++y) {
    for (int x = 0; x < static_cast<int>(width); ++x) {
      const auto out = at(x, y);
      for (int c = 0; c < 4; ++c) {
        double sum = 0.0;
        if (halation) {
          sum = source[at(x, y) + c] * weights_x[0];
          for (int tap = 1; tap < static_cast<int>(weights_x.size()); ++tap) {
            sum += (static_cast<double>(source[at(x + tap, y) + c]) + source[at(x - tap, y) + c]) *
                   weights_x[tap];
          }
        } else {
          sum = source[at(x, y) + c] * static_cast<double>(p.weights[0]);
          for (int tap = 1; tap < static_cast<int>(p.tap_count); ++tap) {
            sum += (static_cast<double>(source[at(x + tap, y) + c]) + source[at(x - tap, y) + c]) *
                   static_cast<double>(p.weights[tap]);
          }
        }
        horizontal[out + c] = sum;
      }
    }
  }
  // Vertical pass, the adjustment, and the mix.
  HostResult result;
  result.pixels.resize(source.size());
  result.near_threshold.assign(static_cast<std::size_t>(width) * height, 0);
  const auto weights_y = HostHalationWeights(p.sigma_y);
  const bool apply_mix = mix != 1.0F || mask != nullptr;
  for (int y = 0; y < static_cast<int>(height); ++y) {
    for (int x = 0; x < static_cast<int>(width); ++x) {
      const auto index = at(x, y);
      double     blur[3]{};
      for (int c = 0; c < 3; ++c) {
        if (halation) {
          blur[c] = horizontal[index + c] * weights_y[0];
          for (int tap = 1; tap < static_cast<int>(weights_y.size()); ++tap) {
            blur[c] +=
                (horizontal[at(x, y - tap) + c] + horizontal[at(x, y + tap) + c]) * weights_y[tap];
          }
        } else {
          blur[c] = horizontal[index + c] * static_cast<double>(p.weights[0]);
          for (int tap = 1; tap < static_cast<int>(p.tap_count); ++tap) {
            blur[c] += (horizontal[at(x, y + tap) + c] + horizontal[at(x, y - tap) + c]) *
                       static_cast<double>(p.weights[tap]);
          }
        }
      }
      const double src[3] = {source[index], source[index + 1], source[index + 2]};
      double       adjusted[3]{src[0], src[1], src[2]};
      if (p.behavior == kSharpen) {
        double     high[3] = {src[0] - blur[0], src[1] - blur[1], src[2] - blur[2]};
        const auto luma    = std::abs(HostLuma(high));
        if (p.threshold > 0.0F) {
          result.near_threshold[index / 4] =
              std::abs(luma - static_cast<double>(p.threshold)) < kThresholdMargin ? 1 : 0;
          if (luma <= p.threshold) {
            high[0] = high[1] = high[2] = 0.0;
          }
        }
        for (int c = 0; c < 3; ++c) {
          adjusted[c] = src[c] + high[c] * p.amount;
        }
      } else if (p.behavior == kClarity) {
        const double diff[3]  = {src[0] - blur[0], src[1] - blur[1], src[2] - blur[2]};
        const double t        = std::clamp(std::abs(HostLuma(diff)) / 0.18, 0.0, 1.0);
        const double protect  = 1.0 - t * t * (3.0 - 2.0 * t);
        const double centered = (HostLuma(src) - 0.5) * 2.0;
        const double strength = p.amount * protect * std::max(1.0 - centered * centered, 0.0);
        for (int c = 0; c < 3; ++c) {
          adjusted[c] = diff[c] * strength + src[c];
        }
      } else if (p.behavior == kHalation) {
        for (int c = 0; c < 3; ++c) {
          adjusted[c] = src[c] + std::max(blur[c] - src[c], 0.0) * p.amount * p.redshift[c];
        }
      }
      double alpha = source[index + 3];
      if (apply_mix) {
        double amount = mix;
        if (mask != nullptr) {
          amount *= static_cast<double>((*mask)[index / 4]) / 255.0;
        }
        amount = std::clamp(amount, 0.0, 1.0);
        for (int c = 0; c < 3; ++c) {
          adjusted[c] = original[index + c] + (adjusted[c] - original[index + c]) * amount;
        }
        alpha = original[index + 3];
      }
      for (int c = 0; c < 3; ++c) {
        result.pixels[index + c] = static_cast<float>(adjusted[c]);
      }
      result.pixels[index + 3] = static_cast<float>(alpha);
    }
  }
  return result;
}

// ------------------------------------------------------------------------------------------------
// Device runs.

enum class SourceKind { CachedImage, WorkBuffer };
enum class DestinationKind { WorkBuffer, SameAsSource, Image };

/// One neighborhood run: the input, the optional mix source and mask, and the scene kinds.
struct NeighborRun {
  const std::vector<float>*        source      = nullptr;
  const std::vector<float>*        original    = nullptr;  ///< null: the source is the mix source
  const std::vector<std::uint8_t>* mask        = nullptr;
  float                            mix         = 1.0F;
  SourceKind                       source_kind = SourceKind::WorkBuffer;
  DestinationKind                  destination_kind = DestinationKind::WorkBuffer;
};

const GraphValueId kSourceId{NodeId{"test.neighborhood.source"}, PortId{"image"}};
const GraphValueId kOriginalId{NodeId{"test.neighborhood.original"}, PortId{"image"}};
const GraphValueId kMaskId{NodeId{"test.neighborhood.mask"}, PortId{"mask"}};
const GraphValueId kDisplayId{NodeId{"test.neighborhood.drt"}, PortId{"display"}};

template <class Device>
void UploadImage(Device& device, const GraphValueId& id, std::span<const std::byte> bytes,
                 std::uint32_t width, std::uint32_t height, TextureFormat format) {
  auto& lease = device.Workspace().AcquireImageForWrite(id, {width, height, format});
  device.Workspace().Device().UploadTexture2D(lease.Texture(), bytes, device.CommandContext());
}

/// Uploads the images of @p run into @p device; returns the source, destination, and mix source.
template <class Device>
auto PrepareScenes(Device& device, const NeighborRun& run, std::uint32_t width,
                   std::uint32_t height) -> std::array<FrameSceneBinding, 3> {
  auto& workspace = device.Workspace();
  workspace.EnsureSceneWorkImages({width, height});
  const auto        source_bytes = std::as_bytes(std::span<const float>(*run.source));
  FrameSceneBinding source;
  if (run.source_kind == SourceKind::CachedImage) {
    UploadImage(device, kSourceId, source_bytes, width, height, TextureFormat::Rgba32f);
    source = FrameSceneBinding::CachedImage(kSourceId);
  } else {
    auto& member = workspace.SceneWork().Member(SceneWorkMember::Member0);
    if constexpr (requires { member.Storage(); }) {
      workspace.Device().UploadBufferRange(member.Storage(), 0, source_bytes,
                                           device.CommandContext());
    } else {
      workspace.Device().UploadTexture2D(member, source_bytes, device.CommandContext());
    }
    source = FrameSceneBinding::WorkImage(SceneWorkMember::Member0);
  }
  FrameSceneBinding destination;
  switch (run.destination_kind) {
    case DestinationKind::WorkBuffer:
      destination = FrameSceneBinding::WorkImage(source.IsWorkImage() ? SceneWorkMember::Member1
                                                                      : SceneWorkMember::Member0);
      break;
    case DestinationKind::SameAsSource:
      destination = source;
      break;
    case DestinationKind::Image:
      (void)workspace.AcquireImageForWrite(kDisplayId, {width, height, TextureFormat::Rgba32f});
      destination = FrameSceneBinding::DisplayImage(kDisplayId);
      break;
  }
  FrameSceneBinding original = source;
  if (run.original != nullptr) {
    UploadImage(device, kOriginalId, std::as_bytes(std::span<const float>(*run.original)), width,
                height, TextureFormat::Rgba32f);
    original = FrameSceneBinding::CachedImage(kOriginalId);
  }
  if (run.mask != nullptr) {
    UploadImage(device, kMaskId, std::as_bytes(std::span<const std::uint8_t>(*run.mask)), width,
                height, TextureFormat::R8);
  }
  return {source, destination, original};
}

auto MakeWork(const GradeNeighborParams& params) -> NeighborWork {
  NeighborWork work;
  work.params        = params;
  work.owner         = NodeId{"test.neighborhood"};
  work.command_index = 0;
  return work;
}

/// The neighborhood of @p params on @p device: the shared NeighborExecutor with VulkanNeighborOps,
/// or the dispatches of OpenClGradeOps and OpenClDrtOps on OpenCL.
template <class Device>
auto RunNeighborhood(Device& device, const GradeNeighborParams& params, const NeighborRun& run,
                     std::uint32_t width, std::uint32_t height) -> std::vector<float> {
  device.BeginRender();
  std::array<FrameSceneBinding, 3> scenes{};
  try {
    scenes                           = PrepareScenes(device, run, width, height);
    const auto& [src, dst, original] = scenes;
    const GraphValueId* mask_id      = run.mask != nullptr ? &kMaskId : nullptr;
    if constexpr (std::is_same_v<Device, VulkanRenderDevice>) {
      NeighborExecutor<VulkanNeighborOps>::Execute(device, src, dst, original, VulkanLutBinding{},
                                                   MakeWork(params), run.mix, mask_id, width,
                                                   height);
    } else {
#if defined(HAVE_OPENCL)
      auto scratch = device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
      EnqueueOpenClNeighborHorizontalScene(device, OpenClBindScene(device, src), scratch.Texture(),
                                           params, width, height);
      const auto original_view = OpenClBindScene(device, original);
      EnqueueOpenClNeighborVerticalScene(device, OpenClBindScene(device, src), scratch.Texture(),
                                         OpenClBindScene(device, dst), &original_view, mask_id,
                                         run.mix, params, width, height);
#endif
    }
    device.EndRender();
    device.WaitIdle();
  } catch (...) {
    device.CancelRender();
    throw;
  }
  auto pixels = DownloadScene(device, scenes[1]);
  device.PublishResults();
  return pixels;
}

/// Sets the texels with @p flags to 0 in both images; returns their count.
auto DropNearThreshold(std::vector<float>& a, std::vector<float>& b, const std::vector<char>& flags)
    -> std::size_t {
  std::size_t count = 0;
  for (std::size_t texel = 0; texel < flags.size(); ++texel) {
    if (flags[texel] == 0) {
      continue;
    }
    ++count;
    for (std::size_t c = 0; c < 4; ++c) {
      a[texel * 4 + c] = 0.0F;
      b[texel * 4 + c] = 0.0F;
    }
  }
  return count;
}

/// Runs @p params on Vulkan and expects the result equal to the host evaluation and, with OpenCL
/// in the build, to the OpenCL kernels on the same GPU.
void ExpectNeighborhoodEqualsReferences(const std::string& label, const GradeNeighborParams& params,
                                        const NeighborRun& run, std::uint32_t width,
                                        std::uint32_t height) {
  VulkanRenderDevice device;
  auto               vulkan = RunNeighborhood(device, params, run, width, height);
  auto host = HostNeighborhood(*run.source, run.original != nullptr ? *run.original : *run.source,
                               run.mask, run.mix, params, width, height);
  auto vulkan_for_host = vulkan;
  const auto dropped   = DropNearThreshold(host.pixels, vulkan_for_host, host.near_threshold);
  EXPECT_LT(dropped, static_cast<std::size_t>(width) * height / 100) << label;
  ExpectEqualToReference(label, host.pixels, vulkan_for_host, width, Uniform(kNeighborTolerance),
                         "host");
#if defined(HAVE_OPENCL)
  ReferenceRenderDevice opencl_device;
  auto                  opencl = RunNeighborhood(opencl_device, params, run, width, height);
  (void)DropNearThreshold(opencl, vulkan, host.near_threshold);
  ExpectEqualToReference(label, opencl, vulkan, width, Uniform(kNeighborTolerance));
#endif
  // The adjustment changed the image: the case tests the kernel, not a copy.
  double change = 0.0;
  for (std::size_t i = 0; i < vulkan.size(); ++i) {
    change = std::max(change, std::abs(static_cast<double>(vulkan[i]) - (*run.source)[i]));
  }
  EXPECT_GT(change, 1.0e-3) << label << ": the output equals the source";
}

class VulkanNeighborhoodTest : public vulkan_workspace_test::VulkanWorkspaceFixture {
 protected:
  void SetUp() override {
    VulkanWorkspaceFixture::SetUp();
#if defined(HAVE_OPENCL)
    const std::string reason = PrepareReferenceBackend();
    ASSERT_TRUE(reason.empty()) << reason;
#endif
  }
};

TEST_F(VulkanNeighborhoodTest, SharpenEqualsTheReferences) {
  const auto  source = MakePlane(kWidth, kHeight, 1);
  NeighborRun run;
  run.source = &source;
  ExpectNeighborhoodEqualsReferences("sharpen 100, radius 1",
                                     SharpenParams(100.0F, 1.0F, 0.0F, kWidth, kHeight), run,
                                     kWidth, kHeight);
  ExpectNeighborhoodEqualsReferences("sharpen 60, radius 3, threshold 0.02",
                                     SharpenParams(60.0F, 3.0F, 0.02F, kWidth, kHeight), run,
                                     kWidth, kHeight);
  const auto largest = SharpenParams(100.0F, 8.0F, 0.0F, kWidth, kHeight);
  EXPECT_EQ(largest.radius, 15U);
  ExpectNeighborhoodEqualsReferences("sharpen 100, radius 8 (15 taps)", largest, run, kWidth,
                                     kHeight);
}

TEST_F(VulkanNeighborhoodTest, ClarityEqualsTheReferences) {
  const auto  source = MakePlane(kWidth, kHeight, 2);
  NeighborRun run;
  run.source          = &source;
  const auto positive = ClarityParams(80.0F, kWidth, kHeight);
  EXPECT_EQ(positive.radius, 45U);
  ExpectNeighborhoodEqualsReferences("clarity +80", positive, run, kWidth, kHeight);
  ExpectNeighborhoodEqualsReferences("clarity -60", ClarityParams(-60.0F, kWidth, kHeight), run,
                                     kWidth, kHeight);
  // The radius is above the image size: most taps read the clamped edge.
  constexpr std::uint32_t kSmallWidth  = 37;
  constexpr std::uint32_t kSmallHeight = 29;
  const auto              small_source = MakePlane(kSmallWidth, kSmallHeight, 3);
  run.source                           = &small_source;
  auto clamped                         = ClarityParams(100.0F, kWidth, kHeight);
  ExpectNeighborhoodEqualsReferences("clarity +100, radius 45 on 37 x 29", clamped, run,
                                     kSmallWidth, kSmallHeight);
}

TEST_F(VulkanNeighborhoodTest, HalationEqualsTheReferences) {
  const auto  source = MakePlane(kWidth, kHeight, 4);
  NeighborRun run;
  run.source          = &source;
  const auto halation = HalationParams(1.0F, kWidth, kHeight);
  EXPECT_FLOAT_EQ(halation.sigma_x, 7.0F);
  ExpectNeighborhoodEqualsReferences("halation 1.0", halation, run, kWidth, kHeight);
  // Different windows on the two axes, and the largest window (radius 63) of the workgroup tile.
  auto anisotropic    = halation;
  anisotropic.sigma_x = 9.0F;
  anisotropic.sigma_y = 2.5F;
  ExpectNeighborhoodEqualsReferences("halation, sigma 9 x 2.5", anisotropic, run, kWidth, kHeight);
  auto widest    = halation;
  widest.sigma_x = 25.0F;
  widest.sigma_y = 25.0F;
  EXPECT_EQ(NeighborhoodVerticalRadius(widest), vulkan::primary_grade_neighbor_kernels::kMaxRadius);
  ExpectNeighborhoodEqualsReferences("halation, sigma 25 (radius 63)", widest, run, kWidth,
                                     kHeight);
}

TEST_F(VulkanNeighborhoodTest, MixAndMaskEqualTheReferences) {
  const auto  source   = MakePlane(kWidth, kHeight, 5);
  const auto  original = MakePlane(kWidth, kHeight, 6);
  const auto  mask     = MakeMask(kWidth, kHeight);
  NeighborRun run;
  run.source   = &source;
  run.original = &original;
  run.mix      = 0.6F;
  ExpectNeighborhoodEqualsReferences(
      "sharpen, mix 0.6", SharpenParams(100.0F, 2.0F, 0.0F, kWidth, kHeight), run, kWidth, kHeight);
  run.mix  = 0.8F;
  run.mask = &mask;
  ExpectNeighborhoodEqualsReferences("clarity, mix 0.8 with a mask",
                                     ClarityParams(70.0F, kWidth, kHeight), run, kWidth, kHeight);
  run.mix = 1.0F;
  ExpectNeighborhoodEqualsReferences("halation, mix 1 with a mask",
                                     HalationParams(0.8F, kWidth, kHeight), run, kWidth, kHeight);
}

TEST_F(VulkanNeighborhoodTest, EachSourceAndDestinationKindGivesTheSameResult) {
  const auto  source = MakePlane(kWidth, kHeight, 7);
  const auto  params = HalationParams(1.0F, kWidth, kHeight);
  NeighborRun run;
  run.source = &source;
  VulkanRenderDevice device;
  const auto         expected = RunNeighborhood(device, params, run, kWidth, kHeight);
  struct Kinds {
    const char*     name;
    SourceKind      source;
    DestinationKind destination;
  };
  const std::vector<Kinds> kinds = {
      {"cached image to a work buffer", SourceKind::CachedImage, DestinationKind::WorkBuffer},
      {"work buffer to itself", SourceKind::WorkBuffer, DestinationKind::SameAsSource},
      {"work buffer to an image", SourceKind::WorkBuffer, DestinationKind::Image},
      {"cached image to an image", SourceKind::CachedImage, DestinationKind::Image},
  };
  for (const auto& kind : kinds) {
    run.source_kind      = kind.source;
    run.destination_kind = kind.destination;
    const auto actual    = RunNeighborhood(device, params, run, kWidth, kHeight);
    ASSERT_EQ(actual.size(), expected.size()) << kind.name;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
      differing += actual[i] != expected[i] ? 1U : 0U;
    }
    EXPECT_EQ(differing, 0U) << kind.name;
  }
}

TEST_F(VulkanNeighborhoodTest, StableNeighborhoodCreatesNoResourceAndRecordsWithoutASubmission) {
  const auto  source = MakePlane(kWidth, kHeight, 8);
  const auto  params = SharpenParams(80.0F, 2.0F, 0.0F, kWidth, kHeight);
  NeighborRun run;
  run.source = &source;
  VulkanRenderDevice device;
  (void)RunNeighborhood(device, params, run, kWidth, kHeight);

  auto& backend = device.Workspace().Device();
  device.BeginRender();
  const auto scenes = PrepareScenes(device, run, kWidth, kHeight);
  backend.ResetCounters();
  NeighborExecutor<VulkanNeighborOps>::Execute(device, scenes[0], scenes[1], scenes[2],
                                               VulkanLutBinding{}, MakeWork(params), 1.0F, nullptr,
                                               kWidth, kHeight);
  EXPECT_EQ(backend.BufferCreateCount(), 0U);
  EXPECT_EQ(backend.TextureCreateCount(), 0U);
  // The only upload is the parameter record of the work.
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 1U);
  EXPECT_EQ(backend.SubmitCount(), 0U);
  EXPECT_EQ(backend.WaitCount(), 0U);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(backend.SubmitCount(), 1U);
  device.PublishResults();
}

TEST_F(VulkanNeighborhoodTest, InvalidRequestsFailBeforeACommand) {
  const auto  source = MakePlane(kWidth, kHeight, 9);
  NeighborRun run;
  run.source = &source;
  VulkanRenderDevice device;
  auto&              backend = device.Workspace().Device();
  device.BeginRender();
  const auto scenes  = PrepareScenes(device, run, kWidth, kHeight);
  auto       scratch = VulkanNeighborOps::AcquireHorizontalScratch(device, kWidth, kHeight);
  backend.ResetCounters();

  const auto expect_invalid = [&](const GradeNeighborParams& params, const char* label) {
    EXPECT_THROW(VulkanNeighborOps::DispatchHorizontal(device, scenes[0], scratch, MakeWork(params),
                                                       kWidth, kHeight),
                 std::invalid_argument)
        << label;
    EXPECT_THROW(VulkanNeighborOps::DispatchVerticalApply(
                     device, scenes[0], scratch, scenes[1], scenes[2], VulkanLutBinding{},
                     MakeWork(params), 1.0F, nullptr, kWidth, kHeight),
                 std::invalid_argument)
        << label;
  };
  FilmGrainModel grain;
  grain.SetValue(0.5F);
  expect_invalid(
      MakeGradeNeighborParams(grain, AdjustmentBehavior::FilmGrain, MakeGeometry(kWidth, kHeight)),
      "film grain");
  auto point     = SharpenParams(50.0F, 1.0F, 0.0F, kWidth, kHeight);
  point.behavior = static_cast<std::uint32_t>(AdjustmentBehavior::Exposure);
  expect_invalid(point, "exposure");
  auto wide      = SharpenParams(50.0F, 1.0F, 0.0F, kWidth, kHeight);
  wide.radius    = 64;
  wide.tap_count = 64;
  expect_invalid(wide, "radius 64");
  auto taps      = SharpenParams(50.0F, 1.0F, 0.0F, kWidth, kHeight);
  taps.tap_count = taps.radius + 2;
  expect_invalid(taps, "taps beyond the radius");
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);

  const auto params    = SharpenParams(50.0F, 1.0F, 0.0F, kWidth, kHeight);
  // The vertical pass of a work without its horizontal pass.
  auto       orphan    = MakeWork(params);
  orphan.command_index = 7;
  EXPECT_THROW(VulkanNeighborOps::DispatchVerticalApply(device, scenes[0], scratch, scenes[1],
                                                        scenes[2], VulkanLutBinding{}, orphan, 1.0F,
                                                        nullptr, kWidth, kHeight),
               std::runtime_error);
  // A scratch image of another size.
  auto narrow = VulkanNeighborOps::AcquireHorizontalScratch(device, kWidth / 2, kHeight);
  EXPECT_THROW(VulkanNeighborOps::DispatchHorizontal(device, scenes[0], narrow, MakeWork(params),
                                                     kWidth, kHeight),
               std::runtime_error);
  // A neighborhood size that differs from the scene images.
  EXPECT_THROW(VulkanNeighborOps::DispatchHorizontal(device, scenes[0], scratch, MakeWork(params),
                                                     kWidth - 1, kHeight),
               std::runtime_error);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
  // A mask that the workspace does not have, after the horizontal pass of the work.
  VulkanNeighborOps::DispatchHorizontal(device, scenes[0], scratch, MakeWork(params), kWidth,
                                        kHeight);
  try {
    VulkanNeighborOps::DispatchVerticalApply(device, scenes[0], scratch, scenes[1], scenes[2],
                                             VulkanLutBinding{}, MakeWork(params), 0.5F, &kMaskId,
                                             kWidth, kHeight);
    ADD_FAILURE() << "a missing mask did not fail";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("mask output is missing"), std::string::npos)
        << error.what();
  }
  device.CancelRender();
}

}  // namespace
}  // namespace alcedo
