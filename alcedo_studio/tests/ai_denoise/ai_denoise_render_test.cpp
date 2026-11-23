//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise model run in the renderer (integration plan, DN6): a test platform through the
// CUDA and OpenCL renderers, and the FP16 graph on DirectML with a RAW file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <opencv2/core.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../edit/graph/test_camera_profile.hpp"
#include "ai_denoise_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/ai_denoise_model_session.hpp"
#include "edit/input/ai_denoise_platform.hpp"
#include "edit/input/raster_input_loader.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "image/image_buffer.hpp"
#include "support/render_snapshot_source.hpp"
#ifdef HAVE_CUDA
#include <cuda_runtime.h>

#include "edit/runtime/cuda/cuda_product_renderer.hpp"
#endif
#ifdef HAVE_OPENCL
#include "edit/runtime/opencl/opencl_renderer.hpp"
#include "opencl/opencl_runtime.hpp"
#endif

namespace alcedo {
namespace {

using ai_denoise_test::BayerPattern;
using ai_denoise_test::MakeBayerInput;
using ai_denoise_test::MakeTestModelSession;
using ai_denoise_test::TestModelFolder;
using ai_denoise_test::TestPlatformRecord;

const std::string kRecordedVersion(64, 'c');

auto              DenoiseDocument(bool on) -> std::shared_ptr<PipelineDocument> {
  auto document = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  gpu_dag_test::EnsureTestCameraProfile(*document);
  if (on) {
    document->Develop()->Params().ApplyAiDenoiseUpdate(DevelopAiDenoise{kRecordedVersion});
  }
  return document;
}

auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(256, tag);
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

/// A test Bayer input of 96 x 64 px for every decode resolution.
auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    auto input = MakeBayerInput(96, 64, BayerPattern("RGGB"));
    if (decode_res == DecodeRes::FULL) {
      return input;
    }
    return RawInputLoader::FromUnpackedCfa(input.pixels, input.cfa_pattern, input.linearization,
                                           input.sensor, decode_res);
  };
}

template <class RendererType>
auto RenderHost(RendererType& renderer, const test::RenderSnapshotSource& source,
                const std::shared_ptr<ImageBuffer>& image, DecodeRes decode_res,
                const RenderRequest& request = {}) -> cv::Mat {
  auto output = renderer.Render(*source.Freeze(), image, decode_res, request, nullptr,
                                FrameCompletionSubmission{}, true);
  if (!output) {
    throw std::runtime_error("the render returned no host output");
  }
  return output->GetCPUData().clone();
}

/// The kind of the AiDenoiseError that @p action throws.
template <typename Action>
auto ErrorKindOf(Action&& action) -> std::optional<AiDenoiseErrorKind> {
  try {
    action();
  } catch (const AiDenoiseError& error) {
    return error.Kind();
  }
  return std::nullopt;
}

template <class RendererType>
void VerifyFullSizeRenderWithTheSwitchOnRendersTheModelOutput() {
  const TestModelFolder folder("render_model_output");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  RendererType renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
  const auto   image = MakeEncodedImage(21);
  const test::RenderSnapshotSource on_source(DenoiseDocument(true));

  const cv::Mat denoised = RenderHost(renderer, on_source, image, DecodeRes::FULL);
  EXPECT_TRUE(cv::checkRange(denoised));
  EXPECT_EQ(session->Stats().denoise_runs, 1U);
  // The cache holds the denoised input of the installed version; a second frame runs no model.
  (void)RenderHost(renderer, on_source, image, DecodeRes::FULL);
  EXPECT_EQ(session->Stats().denoise_runs, 1U);
  auto&      buffer = image->GetBuffer();
  const auto encoded =
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};
  const auto lease = renderer.SourceCache().AcquireDenoised(
      encoded, session->InstalledModelVersion(), [](const PreparedRawInput&) -> PreparedRawInput {
        throw std::logic_error("the denoised entry is not cached");
      });
  EXPECT_EQ(lease.Get().input_kind, RawInputKind::DenoisedRgb);

  // A reduced render (thumbnail, semantic input image) runs no model.
  EXPECT_TRUE(cv::checkRange(RenderHost(renderer, on_source, image, DecodeRes::HALF)));
  EXPECT_EQ(session->Stats().denoise_runs, 1U);

  // The switch off renders the CFA path: other pixels.
  RendererType off_renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
  const test::RenderSnapshotSource off_source(DenoiseDocument(false));
  const cv::Mat cfa = RenderHost(off_renderer, off_source, image, DecodeRes::FULL);
  EXPECT_EQ(session->Stats().denoise_runs, 1U);
  ASSERT_EQ(cfa.type(), denoised.type());
  // The CFA path loses its demosaic border; compare the centre.
  const cv::Rect centre(cfa.cols / 4, cfa.rows / 4, cfa.cols / 2, cfa.rows / 2);
  const cv::Rect denoised_centre(denoised.cols / 4, denoised.rows / 4, cfa.cols / 2, cfa.rows / 2);
  EXPECT_GT(cv::norm(cfa(centre), denoised(denoised_centre), cv::NORM_L1), 0.0);

  // A batch render (export) runs the model at each render.
  RendererType batch(ExecutorRole::Batch, MakeUnpacker(), nullptr, nullptr, session);
  EXPECT_TRUE(cv::checkRange(RenderHost(batch, on_source, image, DecodeRes::FULL)));
  EXPECT_TRUE(cv::checkRange(RenderHost(batch, on_source, image, DecodeRes::FULL)));
  EXPECT_EQ(session->Stats().denoise_runs, 3U);
}

template <class RendererType>
void VerifyModelErrorIsTheRenderErrorWithItsKind() {
  const auto                       image = MakeEncodedImage(22);
  const test::RenderSnapshotSource source(DenoiseDocument(true));
  {
    const TestModelFolder folder("render_not_installed");
    auto                  record  = std::make_shared<TestPlatformRecord>();
    auto                  session = MakeTestModelSession(record, folder);
    std::filesystem::remove(folder.GraphPath());
    RendererType renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
    EXPECT_EQ(ErrorKindOf([&] { (void)RenderHost(renderer, source, image, DecodeRes::FULL); }),
              AiDenoiseErrorKind::ModelNotInstalled);
    EXPECT_EQ(renderer.SourceCache().EntryCount(), 0U);
  }
  {
    const TestModelFolder folder("render_run_failed");
    auto                  record = std::make_shared<TestPlatformRecord>();
    record->failing_tile_run     = 0;
    auto         session         = MakeTestModelSession(record, folder);
    RendererType renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
    EXPECT_EQ(ErrorKindOf([&] { (void)RenderHost(renderer, source, image, DecodeRes::FULL); }),
              AiDenoiseErrorKind::RunFailed);
    // No denoised entry: the next frame runs the model again and renders.
    EXPECT_TRUE(cv::checkRange(RenderHost(renderer, source, image, DecodeRes::FULL)));
    EXPECT_EQ(record->sessions_created.load(), 2);
  }
  {
    const TestModelFolder folder("render_too_slow");
    auto                  record  = std::make_shared<TestPlatformRecord>();
    auto                  session = MakeTestModelSession(record, folder, 0.0);
    RendererType          renderer(ExecutorRole::Batch, MakeUnpacker(), nullptr, nullptr, session);
    EXPECT_EQ(ErrorKindOf([&] { (void)RenderHost(renderer, source, image, DecodeRes::FULL); }),
              AiDenoiseErrorKind::DeviceTooSlow);
  }
}

auto RawAssets() -> std::optional<std::pair<std::filesystem::path, std::filesystem::path>> {
  const char* value = std::getenv("ALCEDO_AI_DENOISE_TEST_ASSETS");
  if (value == nullptr || *value == '\0') {
    return std::nullopt;
  }
  const std::filesystem::path graph = std::filesystem::path(value) / kAiDenoiseGraphFileName;
  const std::filesystem::path raw(ALCEDO_AI_DENOISE_TEST_RAW);
  if (!std::filesystem::exists(graph) || !std::filesystem::exists(raw)) {
    return std::nullopt;
  }
  return std::make_pair(graph, raw);
}

/// Mean absolute 4-neighbour Laplacian of channel 1 of an 8-bit or float RGBA image.
auto MeanAbsoluteLaplacian(const cv::Mat& image) -> double {
  cv::Mat rgba;
  image.convertTo(rgba, CV_32F);
  double sum   = 0.0;
  int    count = 0;
  for (int y = 1; y < rgba.rows - 1; ++y) {
    for (int x = 1; x < rgba.cols - 1; ++x) {
      const auto g = [&](int px, int py) {
        return static_cast<double>(rgba.ptr<float>(py)[px * rgba.channels() + 1]);
      };
      sum += std::abs(4.0 * g(x, y) - g(x - 1, y) - g(x + 1, y) - g(x, y - 1) - g(x, y + 1));
      ++count;
    }
  }
  return sum / count;
}

/// The RAW file through the renderer with the FP16 graph on DirectML (the product platform):
/// the same levels as the CFA render and less noise in a crop at 100%.
template <class RendererType>
void VerifyRawPhotoRendersWithTheFp16GraphOnDirectMl() {
  const auto assets = RawAssets();
  if (!assets) {
    GTEST_SKIP() << "Set ALCEDO_AI_DENOISE_TEST_ASSETS to a folder with the FP16 graph.";
  }
  auto platform = CreateProductAiDenoisePlatform();
  if (!platform) {
    GTEST_SKIP() << "This build has no AI Denoise platform.";
  }
  const TestModelFolder folder("render_raw_photo", "");
  std::filesystem::copy_file(assets->first, folder.GraphPath(),
                             std::filesystem::copy_options::overwrite_existing);
  auto session = std::make_shared<DenoiseModelSession>(std::move(platform));
  session->SetModelFolderProvider([path = folder.Folder()] { return path; });

  std::ifstream             stream(assets->second, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
  const auto                image = std::make_shared<ImageBuffer>(std::move(bytes));
  // A crop of 512 x 512 px at 100% in the centre of the photo.
  RenderRequest             request;
  request.view.visible_rect_in_edit_space = {0.47f, 0.47f, 0.0643f, 0.0964f};
  request.view.viewport_extent            = {512, 512};

  RendererType renderer(ExecutorRole::Interactive, {}, nullptr, nullptr, session);
  const test::RenderSnapshotSource on_source(DenoiseDocument(true));
  const auto                       started = std::chrono::steady_clock::now();
  const cv::Mat denoised = RenderHost(renderer, on_source, image, DecodeRes::FULL, request);
  const auto    seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  EXPECT_EQ(session->Stats().denoise_runs, 1U);
  ASSERT_TRUE(cv::checkRange(denoised));

  RendererType off_renderer(ExecutorRole::Interactive, {}, nullptr, nullptr, session);
  const test::RenderSnapshotSource off_source(DenoiseDocument(false));
  const cv::Mat cfa = RenderHost(off_renderer, off_source, image, DecodeRes::FULL, request);
  ASSERT_EQ(cfa.size(), denoised.size());
  const cv::Scalar cfa_mean      = cv::mean(cfa);
  const cv::Scalar denoised_mean = cv::mean(denoised);
  for (int c = 0; c < 3; ++c) {
    EXPECT_NEAR(denoised_mean[c] / cfa_mean[c], 1.0, 0.05)
        << "channel " << c << ": CFA " << cfa_mean[c] << ", denoised " << denoised_mean[c];
  }
  const double cfa_noise      = MeanAbsoluteLaplacian(cfa);
  const double denoised_noise = MeanAbsoluteLaplacian(denoised);
  std::printf(
      "first render with the model %.2f s; means CFA %.4f %.4f %.4f, denoised %.4f %.4f "
      "%.4f; Laplacian CFA %.5f, denoised %.5f\n",
      seconds, cfa_mean[0], cfa_mean[1], cfa_mean[2], denoised_mean[0], denoised_mean[1],
      denoised_mean[2], cfa_noise, denoised_noise);
  EXPECT_LT(denoised_noise, cfa_noise);

  // The time of a model run on the ready session (the session compiled both tile widths in the
  // first render): the time of a cache miss in the editor (another photo of the same size).
  {
    auto&      encoded   = image->GetBuffer();
    const auto cfa_input = LoadEncodedImage(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(encoded.data()),
                                   encoded.size()},
        DecodeRes::FULL);
    const auto          version = session->InstalledModelVersion();
    std::vector<double> times;
    for (int run = 0; run < 3; ++run) {
      const auto begin  = std::chrono::steady_clock::now();
      const auto result = session->Denoise(cfa_input, version);
      times.push_back(
          std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
      EXPECT_EQ(result.input_kind, RawInputKind::DenoisedRgb);
    }
    std::sort(times.begin(), times.end());
    std::printf("model run on the ready session: %.3f %.3f %.3f s\n", times[0], times[1], times[2]);
  }

  // The editor GPU release ends the session; the next render creates it again.
  session->Release();
  EXPECT_FALSE(session->IsSessionLoaded());
  RendererType second(ExecutorRole::Interactive, {}, nullptr, nullptr, session);
  EXPECT_TRUE(cv::checkRange(RenderHost(second, on_source, image, DecodeRes::FULL, request)));
  EXPECT_EQ(session->Stats().denoise_runs, 5U);
}

#ifdef HAVE_CUDA
auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

TEST(AiDenoiseRenderTest, FullSizeRenderWithTheSwitchOnRendersTheModelOutputOnCuda) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  VerifyFullSizeRenderWithTheSwitchOnRendersTheModelOutput<CudaRenderer>();
}

TEST(AiDenoiseRenderTest, ModelErrorIsTheRenderErrorWithItsKindOnCuda) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  VerifyModelErrorIsTheRenderErrorWithItsKind<CudaRenderer>();
}

TEST(AiDenoiseRenderTest, RawPhotoRendersWithTheFp16GraphOnDirectMlOnCuda) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  VerifyRawPhotoRendersWithTheFp16GraphOnDirectMl<CudaRenderer>();
}
#endif

// DN7: on Windows the executors are in the EditPipeline DLL, which has its own copy of the
// process session. The model folder that the application sets through
// PipelineExecutor::ProcessAiDenoiseSession() must reach the session of the executors.
TEST(AiDenoiseRenderTest, ExecutorsUseTheSessionWhoseModelFolderTheApplicationSets) {
  const ai_denoise_test::TestModelFolder folder("executor_session", "graph of the executors");
  const auto&                            process = PipelineExecutor::ProcessAiDenoiseSession();
  process->SetModelFolderProvider([path = folder.Folder()] { return path; });
  {
    const PipelineExecutor executor(ExecutorRole::Interactive);
    ASSERT_EQ(executor.AiDenoiseSession().get(), process.get());
    EXPECT_EQ(executor.AiDenoiseSession()->InstalledModelVersion(),
              process->InstalledModelVersion());
  }
  process->SetModelFolderProvider({});
}

#ifdef HAVE_OPENCL
TEST(AiDenoiseRenderTest, FullSizeRenderWithTheSwitchOnRendersTheModelOutputOnOpenCl) {
  if (!TryInitializeOpenClRuntime()) GTEST_SKIP() << "No OpenCL device available.";
  VerifyFullSizeRenderWithTheSwitchOnRendersTheModelOutput<OpenClRenderer>();
}

TEST(AiDenoiseRenderTest, ModelErrorIsTheRenderErrorWithItsKindOnOpenCl) {
  if (!TryInitializeOpenClRuntime()) GTEST_SKIP() << "No OpenCL device available.";
  VerifyModelErrorIsTheRenderErrorWithItsKind<OpenClRenderer>();
}

TEST(AiDenoiseRenderTest, RawPhotoRendersWithTheFp16GraphOnDirectMlOnOpenCl) {
  if (!TryInitializeOpenClRuntime()) GTEST_SKIP() << "No OpenCL device available.";
  VerifyRawPhotoRendersWithTheFp16GraphOnDirectMl<OpenClRenderer>();
}
#endif

}  // namespace
}  // namespace alcedo
