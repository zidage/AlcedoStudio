//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "drt_expected_data_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"
#include "edit/runtime/cuda/cuda_drt_runtime_state.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kWidth  = 16;
constexpr std::uint32_t kHeight = 12;

auto                    HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto ExpectedPixelDirectory() -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_DRT_EXPECTED_PIXEL_DIR);
}

void ZeroBytes(std::vector<std::byte>& bytes, std::size_t begin, std::size_t end) {
  std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
            bytes.begin() + static_cast<std::ptrdiff_t>(end), std::byte{0});
}

template <class T>
void AppendDeviceTable(std::vector<std::byte>& out, const CudaDrtTable1D<T>& table) {
  if (table.dev_ptr_ == nullptr) return;
  std::vector<T> host(table.count_);
  ASSERT_EQ(
      ::cudaMemcpy(host.data(), table.dev_ptr_, sizeof(T) * host.size(), cudaMemcpyDeviceToHost),
      cudaSuccess);
  const auto* first = reinterpret_cast<const std::byte*>(host.data());
  out.insert(out.end(), first, first + sizeof(T) * host.size());
}

/**
 * Canonical bytes of a packed CUDA DRT block: the struct with alignment padding, texture handles,
 * device pointers, and host table identities set to zero, followed by the contents of the four
 * device tables (ACES 2.0 only). Padding is excluded because the pre-G10.5 path left it
 * uninitialized; the kernel reads none of those bytes.
 */
auto CanonicalBytes(const CudaDrtGpuParams& packed) -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  drt_expected_data::AppendBytes(bytes, packed);
  constexpr std::size_t kAces = offsetof(CudaDrtGpuParams, aces_params_);
  ZeroBytes(bytes, sizeof(CudaDrtMethod), kAces);
  ZeroBytes(bytes, kAces + offsetof(CudaDrtAcesParams, model_gamma_inv) + sizeof(float),
            kAces + offsetof(CudaDrtAcesParams, table_reach_M_));
  ZeroBytes(bytes, kAces + offsetof(CudaDrtAcesParams, lower_hull_gamma_inv) + sizeof(float),
            kAces + offsetof(CudaDrtAcesParams, table_hues_));
  ZeroBytes(bytes, offsetof(CudaDrtGpuParams, eotf) + sizeof(CudaDrtEotf),
            sizeof(CudaDrtGpuParams));
  const std::size_t tables[] = {offsetof(CudaDrtAcesParams, table_reach_M_),
                                offsetof(CudaDrtAcesParams, table_hues_),
                                offsetof(CudaDrtAcesParams, table_gamut_cusps_),
                                offsetof(CudaDrtAcesParams, table_upper_hull_gamma_)};
  for (const std::size_t table : tables) {
    // texture_object_ and dev_ptr_; count_ stays. The host identity follows the table.
    ZeroBytes(bytes, kAces + table, kAces + table + offsetof(CudaDrtTable1D<float>, count_));
    ZeroBytes(bytes, kAces + table + sizeof(CudaDrtTable1D<float>),
              kAces + table + sizeof(CudaDrtTable1D<float>) + sizeof(std::uintptr_t));
  }
  AppendDeviceTable(bytes, packed.aces_params_.table_reach_M_);
  AppendDeviceTable(bytes, packed.aces_params_.table_hues_);
  AppendDeviceTable(bytes, packed.aces_params_.table_gamut_cusps_);
  AppendDeviceTable(bytes, packed.aces_params_.table_upper_hull_gamma_);
  return bytes;
}

class CudaDrtExpectedOutputFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
    input_ = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(kWidth, kHeight),
                                           gpu_dag_test::FullSensor(kWidth, kHeight));
  }

  /** Render the fixed ramp through the default DAG with @p method and return RGBA32F pixels. */
  auto RenderDisplay(DrtMethod method) -> std::vector<float> {
    auto document  = CreateDefaultPipelineDocument();
    auto payload   = document.Drt()->Params().Params();
    payload.method = method;
    document.Drt()->Params().ReplaceParams(payload);
    gpu_dag_test::EnsureTestCameraProfile(document);
    const auto plan   = GraphCompiler::Compile(document, input_.CompileSource(), RenderRequest{});
    const auto output = device_.Execute(plan, input_, document);
    device_.WaitIdle();
    auto* image = device_.Workspace().Images().Find(output);
    if (image == nullptr || image->Empty()) return {};
    const auto&        texture = image->Texture();
    std::vector<float> pixels(static_cast<std::size_t>(texture.Width()) * texture.Height() * 4U);
    device_.Workspace().Device().DownloadTexture2D(
        texture,
        std::span<std::byte>(reinterpret_cast<std::byte*>(pixels.data()),
                             pixels.size() * sizeof(float)),
        device_.CommandContext());
    return pixels;
  }

  void ExpectMatchesStoredPixels(DrtMethod method, const char* file_name) {
    const auto pixels = RenderDisplay(method);
    ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4U);
    const auto path   = ExpectedPixelDirectory() / file_name;
    const auto stored = drt_expected_data::ReadBytes(path);
    ASSERT_EQ(stored.size(), pixels.size() * sizeof(float)) << path.string();
    std::vector<float> expected(pixels.size());
    std::memcpy(expected.data(), stored.data(), stored.size());
    constexpr float kTolerance = 1.0f / 4096.0f;
    float           max_error  = 0.0f;
    std::size_t     worst      = 0;
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      ASSERT_TRUE(std::isfinite(pixels[i])) << "channel " << i;
      const float error = std::abs(pixels[i] - expected[i]);
      if (error > max_error) {
        max_error = error;
        worst     = i;
      }
    }
    EXPECT_LE(max_error, kTolerance)
        << "channel " << worst << " rendered " << pixels[worst] << " stored " << expected[worst];
  }

  PreparedRawInput input_;
  CudaRenderDevice device_;
};

// The stored pixels were rendered at commit 0cf45f45 (before G10.5) from the 16x12 ramp of
// MakeF32RgbaPlane through the default document. The comparison allows 1/4096 per channel.
TEST_F(CudaDrtExpectedOutputFixture, CudaDrtOutputMatchesStoredExpectedPixels) {
  ExpectMatchesStoredPixels(DrtMethod::Aces20,
                            "cuda_aces20_rec709_gamma22_ramp_16x12_expected_display_rgba32f.bin");
  ExpectMatchesStoredPixels(
      DrtMethod::OpenDrt,
      "cuda_opendrt_standard_rec709_gamma22_ramp_16x12_expected_display_rgba32f.bin");
}

// Primary failure chain: the DRT pass rethrows the resolver message, the render publishes no
// display image, and the same device renders once the parameter is valid again.
TEST_F(CudaDrtExpectedOutputFixture, CudaDrtRejectsUnknownEotfWithoutPublishingDisplay) {
  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  auto payload          = document.Drt()->Params().Params();
  payload.encoding_eotf = static_cast<DrtEotf>(42);
  document.Drt()->Params().ReplaceParams(payload);
  const auto plan = GraphCompiler::Compile(document, input_.CompileSource(), RenderRequest{});
  try {
    (void)device_.Execute(plan, input_, document);
    FAIL() << "CUDA DRT accepted an unknown EOTF";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()),
              "ExecuteCudaDrt: DrtOutputResolver: unsupported DRT encoding EOTF 42.");
  }
  device_.WaitIdle();
  const GraphValueId display{NodeId{"drt"}, PortId{"display"}};
  const auto*        published = device_.Workspace().Images().Find(display);
  EXPECT_TRUE(published == nullptr || published->Empty());

  payload.encoding_eotf = DrtEotf::Gamma22;
  document.Drt()->Params().ReplaceParams(payload);
  const auto output = device_.Execute(plan, input_, document);
  EXPECT_EQ(output, display);
  const auto* rendered = device_.Workspace().Images().Find(display);
  ASSERT_NE(rendered, nullptr);
  EXPECT_FALSE(rendered->Empty());
}

// The stored files were packed through ODT_Op, OperatorParams, and GPUParamsConverter at commit
// 0cf45f45 on a fresh GPUOperatorParams, one configuration per file.
TEST(CudaDrtParameterBytes, CudaDrtParameterBytesMatchStoredExpectedBytes) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  const std::filesystem::path directory(ALCEDO_DRT_EXPECTED_PARAMETER_DIR);
  const auto                  rows = drt_expected_data::ConfigurationMatrix();
  ASSERT_EQ(rows.size(), 12U);
  for (const auto& row : rows) {
    SCOPED_TRACE(row.file_stem_);
    ColorUtils::TO_OUTPUT_Params resolved;
    std::string                  error;
    ASSERT_TRUE(DrtOutputResolver::Resolve(row.payload_, nullptr, &resolved, &error)) << error;
    CudaDrtRuntimeState state;
    const auto          actual = CanonicalBytes(state.Pack(resolved));
    const auto          stored = drt_expected_data::ReadBytes(
        drt_expected_data::ExpectedParameterPath(directory, "cuda", row));
    ASSERT_FALSE(stored.empty());
    EXPECT_EQ(drt_expected_data::FirstDifference(actual, stored), std::string::npos)
        << "actual " << actual.size() << " bytes, stored " << stored.size() << " bytes";
  }
}

// Switching the same device state from ACES 2.0 to OpenDRT and back must pack the same bytes as a
// fresh state; the ACES tables are released and uploaded again.
TEST(CudaDrtParameterBytes, CudaDrtRuntimeStateRepacksAfterMethodSwitch) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  const auto  rows = drt_expected_data::ConfigurationMatrix();
  const auto& aces = rows[0].payload_;
  const auto& open = rows[4].payload_;
  ASSERT_EQ(aces.method, DrtMethod::Aces20);
  ASSERT_EQ(open.method, DrtMethod::OpenDrt);
  ColorUtils::TO_OUTPUT_Params aces_resolved;
  ColorUtils::TO_OUTPUT_Params open_resolved;
  ASSERT_TRUE(DrtOutputResolver::Resolve(aces, nullptr, &aces_resolved, nullptr));
  ASSERT_TRUE(DrtOutputResolver::Resolve(open, nullptr, &open_resolved, nullptr));

  CudaDrtRuntimeState fresh_aces;
  CudaDrtRuntimeState fresh_open;
  const auto          expected_aces = CanonicalBytes(fresh_aces.Pack(aces_resolved));
  const auto          expected_open = CanonicalBytes(fresh_open.Pack(open_resolved));

  CudaDrtRuntimeState reused;
  (void)reused.Pack(aces_resolved);
  const auto& after_open = reused.Pack(open_resolved);
  EXPECT_EQ(after_open.aces_params_.table_hues_.dev_ptr_, nullptr);
  EXPECT_EQ(CanonicalBytes(after_open), expected_open);
  EXPECT_EQ(CanonicalBytes(reused.Pack(aces_resolved)), expected_aces);
}

}  // namespace
}  // namespace alcedo
