//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise model run in the renderer (integration plan, DN6, DN10): a test platform through
// the CUDA, OpenCL, and Metal renderers, and the FP16 graph on the platform of the build (DirectML
// on Windows, Core ML on the Neural Engine on macOS) with a RAW file.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/ai_denoise_model_session.hpp"
#include "edit/input/ai_denoise_platform.hpp"
#include "edit/input/raster_input_loader.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "edit/runtime/render_progress.hpp"
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
#ifdef HAVE_METAL
#include "edit/runtime/metal/metal_renderer.hpp"
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

/// The camera matrix of the gain match of @p document: the matrix of its render at D65 (#1055).
auto DenoiseCamera(const PipelineDocument& document) -> std::array<double, 9> {
  const auto resolved = ResolveDevelopXyzToCameraD65(document.Develop()->Params().Params());
  if (!resolved.ok) {
    throw std::runtime_error("the test document has no camera matrix");
  }
  return resolved.xyz_to_camera;
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
  const auto            image       = MakeEncodedImage(21);
  const auto            on_document = DenoiseDocument(true);
  const test::RenderSnapshotSource on_source(on_document);

  const cv::Mat denoised = RenderHost(renderer, on_source, image, DecodeRes::FULL);
  EXPECT_TRUE(cv::checkRange(denoised));
  EXPECT_EQ(session->Stats().denoise_runs, 1U);
  // The cache holds the denoised input of the installed version and of the camera matrix of the
  // render at D65; a second frame runs no model.
  (void)RenderHost(renderer, on_source, image, DecodeRes::FULL);
  EXPECT_EQ(session->Stats().denoise_runs, 1U);
  auto&      buffer = image->GetBuffer();
  const auto encoded =
      std::span<const std::byte>{reinterpret_cast<const std::byte*>(buffer.data()), buffer.size()};
  const auto lease = renderer.SourceCache().AcquireDenoised(
      encoded, session->InstalledModelVersion(), DenoiseCamera(*on_document),
      [](const PreparedRawInput&) -> PreparedRawInput {
        throw std::logic_error("the denoised entry is not cached");
      });
  EXPECT_EQ(lease.Get().input_kind, RawInputKind::DenoisedRgb);

  // A white balance change keeps the denoised input; another camera profile (another D65 matrix)
  // runs the model again (#1055).
  {
    auto&      develop  = on_document->Develop()->Params();
    const auto original = develop.Params();
    auto       payload  = original;
    payload.wb_mode     = "custom";
    payload.custom_cct  = 3800.0f;
    develop.ReplaceParams(payload);
    EXPECT_TRUE(cv::checkRange(RenderHost(renderer, on_source, image, DecodeRes::FULL)));
    EXPECT_EQ(session->Stats().denoise_runs, 1U);
    payload.camera_profile.color_matrix_2[0] *= 1.05;
    develop.ReplaceParams(payload);
    EXPECT_TRUE(cv::checkRange(RenderHost(renderer, on_source, image, DecodeRes::FULL)));
    EXPECT_EQ(session->Stats().denoise_runs, 2U);
    develop.ReplaceParams(original);
  }

  // A reduced render (thumbnail, semantic input image) runs no model.
  EXPECT_TRUE(cv::checkRange(RenderHost(renderer, on_source, image, DecodeRes::HALF)));
  EXPECT_EQ(session->Stats().denoise_runs, 2U);

  // The switch off renders the CFA path: other pixels.
  RendererType off_renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
  const test::RenderSnapshotSource off_source(DenoiseDocument(false));
  const cv::Mat cfa = RenderHost(off_renderer, off_source, image, DecodeRes::FULL);
  EXPECT_EQ(session->Stats().denoise_runs, 2U);
  ASSERT_EQ(cfa.type(), denoised.type());
  // The CFA path loses its demosaic border; compare the centre.
  const cv::Rect centre(cfa.cols / 4, cfa.rows / 4, cfa.cols / 2, cfa.rows / 2);
  const cv::Rect denoised_centre(denoised.cols / 4, denoised.rows / 4, cfa.cols / 2, cfa.rows / 2);
  EXPECT_GT(cv::norm(cfa(centre), denoised(denoised_centre), cv::NORM_L1), 0.0);

  // A batch render (export) runs the model at each render.
  RendererType batch(ExecutorRole::Batch, MakeUnpacker(), nullptr, nullptr, session);
  EXPECT_TRUE(cv::checkRange(RenderHost(batch, on_source, image, DecodeRes::FULL)));
  EXPECT_TRUE(cv::checkRange(RenderHost(batch, on_source, image, DecodeRes::FULL)));
  EXPECT_EQ(session->Stats().denoise_runs, 4U);
}

/// The values of the render of @p source on @p renderer, at full size with a progress receiver
/// (DN14).
template <class RendererType>
auto RenderWithProgress(RendererType& renderer, const test::RenderSnapshotSource& source,
                        const std::shared_ptr<ImageBuffer>& image) -> std::vector<RenderProgress> {
  std::vector<RenderProgress> values;
  PipelineApplyRequest        request;
  request.decode_res          = DecodeRes::FULL;
  request.role                = ExecutorRole::Interactive;
  request.require_host_output = true;
  request.progress = [&](const RenderProgress& value) { values.push_back(value); };
  if (!renderer.Render(*source.Freeze(), image, request)) {
    throw std::runtime_error("the render returned no host output");
  }
  return values;
}

/// Checks that @p values from @p first on are the Develop groups of one plan, in order.
void ExpectDevelopGroupsInOrder(const std::vector<RenderProgress>& values, std::size_t first) {
  ASSERT_LT(first, values.size());
  const int groups = values[first].total;
  ASSERT_EQ(values.size() - first, static_cast<std::size_t>(groups));
  const int grades = groups - DevelopGroupCount(0);
  ASSERT_GE(grades, 0);
  for (int k = 0; k < groups; ++k) {
    const auto& value = values[first + k];
    SCOPED_TRACE(k);
    EXPECT_EQ(value.step, RenderStep::Develop);
    EXPECT_EQ(value.completed, k);
    EXPECT_EQ(value.total, groups);
    DevelopGroup expected = DevelopGroup::ColorGrade;
    if (k == 0) expected = DevelopGroup::Sensor;
    if (k == 1) expected = DevelopGroup::LensAndGeometry;
    if (k == 2) expected = DevelopGroup::Color;
    if (k == groups - 1) expected = DevelopGroup::Display;
    EXPECT_EQ(value.group, expected);
    if (expected == DevelopGroup::ColorGrade) {
      EXPECT_EQ(value.grade_index, k - 3);
      EXPECT_EQ(value.grade_count, grades);
    }
  }
}

template <class RendererType>
void VerifyRenderThatReadsTheRawReportsItsStepsInOrder() {
  const TestModelFolder folder("render_progress");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  const auto            image   = MakeEncodedImage(23);
  const test::RenderSnapshotSource on_source(DenoiseDocument(true));
  const auto                       off_document = DenoiseDocument(false);
  const test::RenderSnapshotSource off_source(off_document);

  // The first frame of a photo with the switch on: Reading RAW, the tiles, the Develop groups.
  RendererType renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
  const auto   first = RenderWithProgress(renderer, on_source, image);
  ASSERT_GE(first.size(), 3U);
  for (const auto& value : first) {
    EXPECT_TRUE(value.read_raw);
    EXPECT_TRUE(value.ai_denoise);
  }
  EXPECT_EQ(first[0].step, RenderStep::ReadRaw);
  EXPECT_EQ(first[1].step, RenderStep::AiDenoise);
  EXPECT_EQ(first[1].completed, 0);
  EXPECT_EQ(first[1].total, 0);
  std::size_t index = 2;
  int         tiles = 0;
  while (index < first.size() && first[index].step == RenderStep::AiDenoise) {
    ++tiles;
    EXPECT_EQ(first[index].completed, tiles);
    ++index;
  }
  ASSERT_GT(tiles, 0);
  EXPECT_EQ(first[index - 1].total, tiles);
  ExpectDevelopGroupsInOrder(first, index);

  // The next frame reads the caches: it reports nothing.
  EXPECT_TRUE(RenderWithProgress(renderer, on_source, image).empty());

  // A photo with the switch off: Reading RAW and the Develop groups.
  RendererType off_renderer(ExecutorRole::Interactive, MakeUnpacker(), nullptr, nullptr, session);
  const auto   off = RenderWithProgress(off_renderer, off_source, image);
  ASSERT_GE(off.size(), 2U);
  EXPECT_TRUE(off[0].read_raw);
  EXPECT_FALSE(off[0].ai_denoise);
  EXPECT_EQ(off[0].step, RenderStep::ReadRaw);
  ExpectDevelopGroupsInOrder(off, 1);
  EXPECT_TRUE(RenderWithProgress(off_renderer, off_source, image).empty());

  // The switch goes on in the open photo (the same document; another document is another
  // binding, whose renderer clears its caches): the source is in the cache, so no Reading RAW.
  off_document->Develop()->Params().ApplyAiDenoiseUpdate(DevelopAiDenoise{kRecordedVersion});
  const auto switched = RenderWithProgress(off_renderer, off_source, image);
  ASSERT_GE(switched.size(), 2U);
  EXPECT_FALSE(switched[0].read_raw);
  EXPECT_TRUE(switched[0].ai_denoise);
  EXPECT_EQ(switched[0].step, RenderStep::AiDenoise);
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
  const std::filesystem::path graph =
      std::filesystem::path(value) / ai_denoise_test::kFp16GraphFileName;
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

/// The RAW file through the renderer with the FP16 graph on the product platform (DirectML,
/// Core ML): the same levels as the CFA render and less noise in a crop at 100%.
template <class RendererType>
void VerifyRawPhotoRendersWithTheFp16GraphOfThePlatform() {
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
  session->SetGraphLocator(folder.Locator());

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

  // The time of a model run on the ready session (the session compiled the tile shapes in the
  // first render): the time of a cache miss in the editor (another photo of the same size).
  {
    auto&      encoded   = image->GetBuffer();
    const auto cfa_input = LoadEncodedImage(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(encoded.data()),
                                   encoded.size()},
        DecodeRes::FULL);
    const auto          version = session->InstalledModelVersion();
    const auto          camera  = DenoiseCamera(*DenoiseDocument(true));
    std::vector<double> times;
    for (int run = 0; run < 3; ++run) {
      const auto begin  = std::chrono::steady_clock::now();
      const auto result = session->Denoise(cfa_input, version, camera);
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

TEST(AiDenoiseRenderTest, RenderThatReadsTheRawReportsItsStepsInOrderOnCuda) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  VerifyRenderThatReadsTheRawReportsItsStepsInOrder<CudaRenderer>();
}

TEST(AiDenoiseRenderTest, RawPhotoRendersWithTheFp16GraphOnDirectMlOnCuda) {
  if (!HasCudaDevice()) GTEST_SKIP() << "No CUDA device available.";
  VerifyRawPhotoRendersWithTheFp16GraphOfThePlatform<CudaRenderer>();
}
#endif

// DN7: on Windows the executors are in the EditPipeline DLL, which has its own copy of the
// process session. The graph locator that the application sets through
// PipelineExecutor::ProcessAiDenoiseSession() must reach the session of the executors.
TEST(AiDenoiseRenderTest, ExecutorsUseTheSessionWhoseGraphLocatorTheApplicationSets) {
  const ai_denoise_test::TestModelFolder folder("executor_session", "graph of the executors");
  const auto&                            process = PipelineExecutor::ProcessAiDenoiseSession();
  process->SetGraphLocator(folder.Locator());
  {
    const PipelineExecutor executor(ExecutorRole::Interactive);
    ASSERT_EQ(executor.AiDenoiseSession().get(), process.get());
    EXPECT_EQ(executor.AiDenoiseSession()->InstalledModelVersion(),
              process->InstalledModelVersion());
  }
  process->SetGraphLocator({});
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

TEST(AiDenoiseRenderTest, RenderThatReadsTheRawReportsItsStepsInOrderOnOpenCl) {
  if (!TryInitializeOpenClRuntime()) GTEST_SKIP() << "No OpenCL device available.";
  VerifyRenderThatReadsTheRawReportsItsStepsInOrder<OpenClRenderer>();
}

TEST(AiDenoiseRenderTest, RawPhotoRendersWithTheFp16GraphOnDirectMlOnOpenCl) {
  if (!TryInitializeOpenClRuntime()) GTEST_SKIP() << "No OpenCL device available.";
  VerifyRawPhotoRendersWithTheFp16GraphOfThePlatform<OpenClRenderer>();
}
#endif

#ifdef HAVE_METAL
auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

TEST(AiDenoiseRenderTest, FullSizeRenderWithTheSwitchOnRendersTheModelOutputOnMetal) {
  if (!HasMetalDevice()) GTEST_SKIP() << "No Metal device available.";
  VerifyFullSizeRenderWithTheSwitchOnRendersTheModelOutput<MetalRenderer>();
}

TEST(AiDenoiseRenderTest, ModelErrorIsTheRenderErrorWithItsKindOnMetal) {
  if (!HasMetalDevice()) GTEST_SKIP() << "No Metal device available.";
  VerifyModelErrorIsTheRenderErrorWithItsKind<MetalRenderer>();
}

TEST(AiDenoiseRenderTest, RenderThatReadsTheRawReportsItsStepsInOrderOnMetal) {
  if (!HasMetalDevice()) GTEST_SKIP() << "No Metal device available.";
  VerifyRenderThatReadsTheRawReportsItsStepsInOrder<MetalRenderer>();
}

TEST(AiDenoiseRenderTest, RawPhotoRendersWithTheFp16GraphOnCoreMlOnMetal) {
  if (!HasMetalDevice()) GTEST_SKIP() << "No Metal device available.";
  VerifyRawPhotoRendersWithTheFp16GraphOfThePlatform<MetalRenderer>();
}
#endif

}  // namespace
}  // namespace alcedo
