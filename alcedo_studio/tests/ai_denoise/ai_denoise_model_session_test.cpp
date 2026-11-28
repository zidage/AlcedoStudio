//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the owner of the AI Denoise model session and of the denoised entry of the prepared
// source cache (integration plan, DN6), with a test platform.

#include "edit/input/ai_denoise_model_session.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ai_denoise_test_support.hpp"
#include "edit/input/ai_denoise_input.hpp"
#include "edit/input/prepared_source_cache.hpp"
#include "edit/input/raster_input_loader.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

using ai_denoise_test::BayerPattern;
using ai_denoise_test::kTestXyzToCamera;
using ai_denoise_test::MakeBayerInput;
using ai_denoise_test::MakeTestModelSession;
using ai_denoise_test::TestModelFolder;
using ai_denoise_test::TestPlatformRecord;

auto Sha256Of(const std::string& text) -> std::string {
  return ComputeSha256Hex(std::as_bytes(std::span(text.data(), text.size())));
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

TEST(AiDenoiseModelSessionTest, InstalledModelVersionIsTheSha256OfTheGraphFile) {
  const TestModelFolder folder("installed_version", "graph v1");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  EXPECT_EQ(session->InstalledModelVersion(), Sha256Of("graph v1"));
  EXPECT_EQ(session->InstalledModelVersion(), Sha256Of("graph v1"));
  // A new file of another size is read again.
  folder.Write("graph version 2");
  EXPECT_EQ(session->InstalledModelVersion(), Sha256Of("graph version 2"));
}

TEST(AiDenoiseModelSessionTest, MissingGraphReportsModelNotInstalled) {
  auto record = std::make_shared<TestPlatformRecord>();
  {
    const TestModelFolder folder("missing_graph");
    auto                  session = MakeTestModelSession(record, folder);
    std::filesystem::remove(folder.GraphPath());
    EXPECT_EQ(ErrorKindOf([&] { (void)session->InstalledModelVersion(); }),
              AiDenoiseErrorKind::ModelNotInstalled);
    EXPECT_EQ(ErrorKindOf([&] {
                (void)session->Denoise(MakeBayerInput(32, 32, BayerPattern("RGGB")),
                                       std::string(64, 'a'), kTestXyzToCamera);
              }),
              AiDenoiseErrorKind::RunFailed);
  }
  // No model folder at all.
  DenoiseModelSession unset(std::make_unique<ai_denoise_test::TestPlatform>(record));
  EXPECT_EQ(ErrorKindOf([&] { (void)unset.InstalledModelVersion(); }),
            AiDenoiseErrorKind::ModelNotInstalled);
  EXPECT_EQ(record->sessions_created.load(), 0);
}

TEST(AiDenoiseModelSessionTest, DenoiseGivesTheGainMatchedInputOfTheDecodeArea) {
  const TestModelFolder folder("gain_matched");
  auto                  record   = std::make_shared<TestPlatformRecord>();
  auto                  session  = MakeTestModelSession(record, folder);
  const auto            version  = session->InstalledModelVersion();
  // GRBG with an odd size: a column at the left and a row at the bottom are added. Packed 300 x
  // 102: two tiles of 256.
  const auto            input    = MakeBayerInput(599, 203, BayerPattern("GRBG"));
  const auto            denoised = session->Denoise(input, version, kTestXyzToCamera);

  EXPECT_EQ(denoised.input_kind, RawInputKind::DenoisedRgb);
  EXPECT_EQ(denoised.host_extent.width, 599U);
  EXPECT_EQ(denoised.host_extent.height, 203U);
  EXPECT_EQ(denoised.source_key.denoise_hash, HashAiDenoiseRun(version, kTestXyzToCamera));
  EXPECT_GT(record->tile_runs.load(), 1);

  // The expected plane: the test output (the packed input times 1e5 at each 2 x 2 cell) with the
  // gain match factor of the means of the area.
  const auto         mosaic = PackBayerForAiDenoise(input);
  const int          width  = mosaic.OutputWidth();
  const int          height = mosaic.OutputHeight();
  std::vector<float> output(static_cast<std::size_t>(3) * width * height);
  const auto         plane = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  for (int c = 0; c < 3; ++c) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        output[(static_cast<std::size_t>(c) * height + y) * width + x] = static_cast<float>(
            mosaic.planes[ai_denoise_test::kChannelOfOutput[c] * plane +
                          static_cast<std::size_t>(y / 2) * mosaic.width + x / 2] *
            ai_denoise_test::kTestOutputScale);
      }
    }
  }
  const nn::RgbPlanesView view{output, width, height};
  AiDenoiseOutputSums     sums(mosaic);
  sums.AddKept(view, {0, 0, mosaic.width, mosaic.height});
  const double gain =
      AiDenoiseGainFactor(AiDenoiseGainWeights(kTestXyzToCamera), mosaic.input_means, sums.Means());
  const auto   expected = AssembleDenoisedPlane(view, mosaic, gain, input.linearization);
  const auto*  actual   = reinterpret_cast<const float*>(denoised.pixels.bytes.get());
  const auto*  wanted   = reinterpret_cast<const float*>(expected.bytes.get());
  double       largest  = 0.0;
  for (std::size_t i = 0; i < expected.ByteCount() / sizeof(float); ++i) {
    largest = std::max(largest, std::abs(static_cast<double>(actual[i]) - wanted[i]));
  }
  // The tile sums and the sum of one pass add in another order.
  EXPECT_LE(largest, 1e-6) << largest;
}

TEST(AiDenoiseModelSessionTest, DenoiseOfADarkPhotoReturnsItsExposureAfterTheInputCurve) {
  const TestModelFolder folder("dark_photo");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  const auto            version = session->InstalledModelVersion();
  // RGGB with even sizes: every 2 x 2 cell is inside the area, so the output of the test session
  // (the R, G1, and B sample of each cell) has the means of the input.
  const auto            input   = ai_denoise_test::MakeDarkBayerInput(600, 204);
  const auto            mosaic  = PackBayerForAiDenoise(input);
  const auto            curve   = AiDenoiseInputCurveFor(mosaic.input_means);
  ASSERT_GT(curve.gain, 4.0);
  const auto denoised = session->Denoise(input, version, kTestXyzToCamera);
  EXPECT_EQ(session->Stats().last_input_gain, curve.gain);

  // The test session returns its input, so the result is the prepared input of each cell in the
  // white level domain: the inverse curve returns the exposure of the photo.
  const auto* actual    = reinterpret_cast<const float*>(denoised.pixels.bytes.get());
  const auto& lin       = input.linearization;
  const auto  plane     = static_cast<std::size_t>(mosaic.width) * mosaic.height;
  double      largest   = 0.0;
  bool        shoulder  = false;
  bool        above_one = false;
  for (int y = 0; y < 204; ++y) {
    for (int x = 0; x < 600; ++x) {
      for (int c = 0; c < 3; ++c) {
        const double prepared =
            mosaic.planes[ai_denoise_test::kChannelOfOutput[c] * plane +
                          static_cast<std::size_t>(y / 2) * mosaic.width + x / 2];
        const double scale = (lin.libraw_maximum - lin.black_level[c]) /
                             (static_cast<double>(lin.white_level[c]) - lin.black_level[c]);
        const double expected = prepared * scale;
        const double value    = actual[(static_cast<std::size_t>(y) * 600 + x) * 4 + c];
        largest   = std::max(largest, std::abs(value - expected) / (std::abs(expected) + 1e-3));
        shoulder  = shoulder || curve.Apply(prepared) > kAiDenoiseCurveKnee;
        above_one = above_one || prepared > 1.0;
      }
    }
  }
  EXPECT_TRUE(shoulder);
  EXPECT_TRUE(above_one);
  EXPECT_LE(largest, 1e-5) << largest;

  // A photo at the training level runs without the curve.
  (void)session->Denoise(MakeBayerInput(64, 48, BayerPattern("RGGB")), version, kTestXyzToCamera);
  EXPECT_EQ(session->Stats().last_input_gain, 1.0);
}

// DN14: the editor shows tile k of n of the run.
TEST(AiDenoiseModelSessionTest, DenoiseReportsEachTileOfTheRun) {
  const TestModelFolder folder("tile_progress");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  const auto            version = session->InstalledModelVersion();
  // 600 x 400 px: 300 x 200 packed px in tiles of 256 with an overlap of 96 is more than one tile.
  const auto            input   = MakeBayerInput(600, 400, BayerPattern("RGGB"));

  std::vector<std::pair<int, int>> reports;
  (void)session->Denoise(input, version, kTestXyzToCamera, [&](int completed, int total) {
    reports.emplace_back(completed, total);
  });
  ASSERT_GT(reports.size(), 1U);
  const int total = reports.front().second;
  ASSERT_EQ(reports.size(), static_cast<std::size_t>(total));
  for (int k = 0; k < total; ++k) {
    EXPECT_EQ(reports[k], std::make_pair(k + 1, total));
  }

  // A run without a receiver reports nothing and gives the same result.
  (void)session->Denoise(input, version, kTestXyzToCamera);
  EXPECT_EQ(session->Stats().denoise_runs, 2U);
}

TEST(AiDenoiseModelSessionTest, ModelSessionCreatesThePlatformSessionOnceAndAgainAfterRelease) {
  const TestModelFolder folder("session_lifetime");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  const auto            version = session->InstalledModelVersion();
  const auto            input   = MakeBayerInput(64, 48, BayerPattern("RGGB"));
  EXPECT_FALSE(session->IsSessionLoaded());

  (void)session->Denoise(input, version, kTestXyzToCamera);
  (void)session->Denoise(input, version, kTestXyzToCamera);
  EXPECT_EQ(record->sessions_created.load(), 1);
  EXPECT_EQ(record->sessions_alive.load(), 1);
  EXPECT_TRUE(session->IsSessionLoaded());
  EXPECT_EQ(session->Stats().denoise_runs, 2U);

  session->Release();
  EXPECT_FALSE(session->IsSessionLoaded());
  EXPECT_EQ(record->sessions_alive.load(), 0);

  (void)session->Denoise(input, version, kTestXyzToCamera);
  EXPECT_EQ(record->sessions_created.load(), 2);

  // Another installed model version loads the graph again.
  (void)session->Denoise(input, std::string(64, 'e'), kTestXyzToCamera);
  EXPECT_EQ(record->sessions_created.load(), 3);
  EXPECT_EQ(record->sessions_alive.load(), 1);
}

TEST(AiDenoiseModelSessionTest, ModelSessionRunsOneDenoiseAtATime) {
  const TestModelFolder folder("one_run_at_a_time");
  auto                  record     = std::make_shared<TestPlatformRecord>();
  record->run_sleep                = std::chrono::milliseconds(5);
  auto                     session = MakeTestModelSession(record, folder);
  const auto               version = session->InstalledModelVersion();
  const auto               input   = MakeBayerInput(400, 300, BayerPattern("RGGB"));
  std::vector<std::thread> threads;
  for (int i = 0; i < 3; ++i) {
    threads.emplace_back([&] { (void)session->Denoise(input, version, kTestXyzToCamera); });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(record->most_active_runs.load(), 1);
  EXPECT_EQ(session->Stats().denoise_runs, 3U);
  EXPECT_EQ(record->sessions_created.load(), 1);
}

TEST(AiDenoiseModelSessionTest, SlowDeviceResultIsKeptForTheDeviceAfterRelease) {
  const TestModelFolder folder("slow_device");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  // A limit of 0 s: each measured tile is above it.
  auto                  session = MakeTestModelSession(record, folder, 0.0);
  const auto            version = session->InstalledModelVersion();
  const auto            input   = MakeBayerInput(400, 300, BayerPattern("RGGB"));

  EXPECT_EQ(ErrorKindOf([&] { (void)session->Denoise(input, version, kTestXyzToCamera); }),
            AiDenoiseErrorKind::DeviceTooSlow);
  EXPECT_EQ(record->sessions_created.load(), 1);
  EXPECT_EQ(record->tile_runs.load(), 1);
  EXPECT_FALSE(session->IsSessionLoaded());

  session->Release();
  // A later run on that device stops at once: no session, no tile.
  EXPECT_EQ(ErrorKindOf([&] { (void)session->Denoise(input, version, kTestXyzToCamera); }),
            AiDenoiseErrorKind::DeviceTooSlow);
  EXPECT_EQ(record->sessions_created.load(), 1);
  EXPECT_EQ(record->tile_runs.load(), 1);

  // Another device runs its tiles.
  record->device_key = "test device B";
  EXPECT_EQ(ErrorKindOf([&] { (void)session->Denoise(input, version, kTestXyzToCamera); }),
            AiDenoiseErrorKind::DeviceTooSlow);
  EXPECT_EQ(record->sessions_created.load(), 2);
  EXPECT_EQ(record->tile_runs.load(), 2);
}

TEST(AiDenoiseModelSessionTest, RunErrorReleasesThePlatformSessionAndReportsRunFailed) {
  const TestModelFolder folder("run_error");
  auto                  record = std::make_shared<TestPlatformRecord>();
  record->failing_tile_run     = 0;
  auto       session           = MakeTestModelSession(record, folder);
  const auto version           = session->InstalledModelVersion();
  const auto input             = MakeBayerInput(400, 300, BayerPattern("RGGB"));
  try {
    (void)session->Denoise(input, version, kTestXyzToCamera);
    ADD_FAILURE() << "the run error was not reported";
  } catch (const AiDenoiseError& error) {
    EXPECT_EQ(error.Kind(), AiDenoiseErrorKind::RunFailed);
    EXPECT_NE(std::string(error.what()).find("887A0005"), std::string::npos) << error.what();
  }
  EXPECT_FALSE(session->IsSessionLoaded());
  EXPECT_EQ(record->sessions_alive.load(), 0);
  // The next run creates the session again.
  (void)session->Denoise(input, version, kTestXyzToCamera);
  EXPECT_EQ(record->sessions_created.load(), 2);
}

// #1055: LibRaw leaves `cam_xyz` zero for many DNG files (it uses the DNG ColorMatrix only for
// rgb_cam) and for cameras newer than its table. The run takes the camera matrix of the render.
TEST(AiDenoiseModelSessionTest, PhotoWithoutLibRawCamXyzDenoisesWithTheCameraMatrixOfTheRender) {
  const std::filesystem::path files[] = {ALCEDO_AI_DENOISE_TEST_DNG_WITHOUT_CAM_XYZ,
                                         ALCEDO_AI_DENOISE_TEST_RAW_WITHOUT_CAM_XYZ};
  const TestModelFolder       folder("without_cam_xyz");
  auto                        record  = std::make_shared<TestPlatformRecord>();
  auto                        session = MakeTestModelSession(record, folder);
  const auto                  version = session->InstalledModelVersion();
  int                         tested  = 0;
  for (const auto& path : files) {
    if (!std::filesystem::exists(path)) {
      continue;
    }
    SCOPED_TRACE(path.string());
    ASSERT_TRUE(ai_denoise_test::LibRawCamXyzIsSingular(path));
    std::ifstream     stream(path, std::ios::binary);
    std::vector<char> encoded((std::istreambuf_iterator<char>(stream)),
                              std::istreambuf_iterator<char>());
    const auto        input =
        LoadEncodedImage(std::as_bytes(std::span(encoded.data(), encoded.size())), DecodeRes::FULL);
    ASSERT_EQ(input.input_kind, RawInputKind::BayerRaw);

    const auto camera  = ai_denoise_test::ImportedXyzToCamera(path);
    const auto weights = AiDenoiseGainWeights(camera);
    EXPECT_GT(weights[0] + weights[1] + weights[2], 0.0);
    const auto denoised = session->Denoise(input, version, camera);
    EXPECT_EQ(denoised.input_kind, RawInputKind::DenoisedRgb);
    EXPECT_EQ(denoised.source_key.denoise_hash, HashAiDenoiseRun(version, camera));
    ++tested;
  }
  if (tested == 0) {
    GTEST_SKIP() << "The sample photos without a LibRaw cam_xyz are not in this checkout.";
  }
}

TEST(AiDenoiseModelSessionTest, PlatformWithoutForwardCodeReportsRunFailed) {
  const TestModelFolder folder("no_platform");
  DenoiseModelSession   session(nullptr);
  session.SetGraphLocator(folder.Locator());
  const auto version = session.InstalledModelVersion();
  EXPECT_EQ(ErrorKindOf([&] {
              (void)session.Denoise(MakeBayerInput(32, 32, BayerPattern("RGGB")), version,
                                    kTestXyzToCamera);
            }),
            AiDenoiseErrorKind::RunFailed);
}

// The denoised entry of the prepared source cache.

auto EncodedBytes(const std::vector<std::byte>& bytes) -> std::span<const std::byte> {
  return {bytes.data(), bytes.size()};
}

TEST(AiDenoiseModelSessionTest, DenoisedEntryIsReusedAndRunsTheModelOnce) {
  int                          unpacks = 0;
  PreparedSourceCache          cache([&](std::span<const std::byte>, DecodeRes decode_res) {
    ++unpacks;
    EXPECT_EQ(decode_res, DecodeRes::FULL);
    return MakeBayerInput(64, 48, BayerPattern("RGGB"));
  });
  const std::vector<std::byte> encoded(128, std::byte{7});
  int                          runs    = 0;
  const auto                   denoise = [&](const PreparedRawInput& cfa) {
    ++runs;
    // A plane of the decode area, one value.
    HostImagePlane rgba;
    rgba.extent       = Extent2D{64, 48};
    rgba.stride_bytes = 64 * 16;
    rgba.format       = HostPixelFormat::F32Rgba;
    auto storage = std::shared_ptr<std::byte>(new std::byte[rgba.ByteCount()],
                                                                [](std::byte* p) { delete[] p; });
    std::fill_n(reinterpret_cast<float*>(storage.get()), rgba.ByteCount() / 4, 0.25f);
    rgba.bytes = std::const_pointer_cast<const std::byte>(storage);
    return RawInputLoader::FromDenoisedBayer(cfa, std::move(rgba), std::string(64, 'a'),
                                             kTestXyzToCamera);
  };
  const std::string version_a(64, 'a');
  const std::string version_b(64, 'b');

  {
    const auto lease =
        cache.AcquireDenoised(EncodedBytes(encoded), version_a, kTestXyzToCamera, denoise);
    EXPECT_EQ(lease.Get().input_kind, RawInputKind::DenoisedRgb);
  }
  {
    const auto lease =
        cache.AcquireDenoised(EncodedBytes(encoded), version_a, kTestXyzToCamera, denoise);
    EXPECT_EQ(lease.Get().input_kind, RawInputKind::DenoisedRgb);
  }
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(unpacks, 1);
  EXPECT_EQ(cache.GetStats().denoise_runs, 1U);

  // Another installed model version is another entry; the CFA entry is still cached.
  (void)cache.AcquireDenoised(EncodedBytes(encoded), version_b, kTestXyzToCamera, denoise);
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(unpacks, 1);

  // A budget of one denoised entry: the unleased CFA entry is evicted before the denoised
  // entries, which keep the most recent one.
  const std::size_t denoised_bytes = std::size_t{64} * 48 * 16;
  cache.SetHostByteBudget(denoised_bytes);
  EXPECT_EQ(cache.EntryCount(), 1U);
  (void)cache.AcquireDenoised(EncodedBytes(encoded), version_b, kTestXyzToCamera, denoise);
  EXPECT_EQ(runs, 2);
  // The switch off: the CFA entry is unpacked again.
  (void)cache.AcquireEncoded(EncodedBytes(encoded), DecodeRes::FULL);
  EXPECT_EQ(unpacks, 2);

  // Another camera matrix of the gain match (another camera profile) is another entry (#1055).
  auto other_camera = kTestXyzToCamera;
  other_camera[0] += 0.05;
  (void)cache.AcquireDenoised(EncodedBytes(encoded), version_b, other_camera, denoise);
  EXPECT_EQ(runs, 3);
  EXPECT_EQ(unpacks, 2);
}

// DN14: the renderer asks the cache which steps a render has before it acquires the source.
TEST(AiDenoiseModelSessionTest, CacheReportsWhetherAnAcquireReadsTheRawOrRunsTheModel) {
  PreparedSourceCache cache([](std::span<const std::byte>, DecodeRes) {
    return MakeBayerInput(64, 48, BayerPattern("RGGB"));
  });
  const std::vector<std::byte> encoded(128, std::byte{9});
  const std::string            version(64, 'a');
  const auto                   denoise = [&](const PreparedRawInput& cfa) {
    HostImagePlane rgba;
    rgba.extent       = Extent2D{64, 48};
    rgba.stride_bytes = 64 * 16;
    rgba.format       = HostPixelFormat::F32Rgba;
    auto storage      = std::shared_ptr<std::byte>(new std::byte[rgba.ByteCount()],
                                                   [](std::byte* p) { delete[] p; });
    std::fill_n(reinterpret_cast<float*>(storage.get()), rgba.ByteCount() / 4, 0.25f);
    rgba.bytes = std::const_pointer_cast<const std::byte>(storage);
    return RawInputLoader::FromDenoisedBayer(cfa, std::move(rgba), version, kTestXyzToCamera);
  };

  EXPECT_FALSE(cache.HoldsEncoded(EncodedBytes(encoded), DecodeRes::FULL));
  EXPECT_FALSE(cache.HoldsDenoised(EncodedBytes(encoded), version, kTestXyzToCamera));
  EXPECT_EQ(cache.EntryCount(), 0U);
  EXPECT_EQ(cache.GetStats().misses, 0U);

  (void)cache.AcquireEncoded(EncodedBytes(encoded), DecodeRes::FULL);
  EXPECT_TRUE(cache.HoldsEncoded(EncodedBytes(encoded), DecodeRes::FULL));
  EXPECT_FALSE(cache.HoldsEncoded(EncodedBytes(encoded), DecodeRes::HALF));
  EXPECT_FALSE(cache.HoldsDenoised(EncodedBytes(encoded), version, kTestXyzToCamera));

  (void)cache.AcquireDenoised(EncodedBytes(encoded), version, kTestXyzToCamera, denoise);
  EXPECT_TRUE(cache.HoldsDenoised(EncodedBytes(encoded), version, kTestXyzToCamera));
  EXPECT_FALSE(cache.HoldsDenoised(EncodedBytes(encoded), std::string(64, 'b'), kTestXyzToCamera));
  auto other_camera = kTestXyzToCamera;
  other_camera[4] += 0.05;
  EXPECT_FALSE(cache.HoldsDenoised(EncodedBytes(encoded), version, other_camera));

  // The queries change no stats and no entry.
  const auto stats = cache.GetStats();
  (void)cache.HoldsEncoded(EncodedBytes(encoded), DecodeRes::FULL);
  (void)cache.HoldsDenoised(EncodedBytes(encoded), version, kTestXyzToCamera);
  EXPECT_EQ(cache.GetStats().hits, stats.hits);
  EXPECT_EQ(cache.GetStats().misses, stats.misses);
  EXPECT_EQ(cache.EntryCount(), 2U);

  cache.Clear();
  EXPECT_FALSE(cache.HoldsEncoded(EncodedBytes(encoded), DecodeRes::FULL));
  EXPECT_FALSE(cache.HoldsDenoised(EncodedBytes(encoded), version, kTestXyzToCamera));
}

TEST(AiDenoiseModelSessionTest, DenoiseHashCoversTheModelVersionAndTheCameraMatrix) {
  const std::string version(64, 'a');
  auto              other_camera = kTestXyzToCamera;
  other_camera[4] -= 0.01;
  const auto hash = HashAiDenoiseRun(version, kTestXyzToCamera);
  EXPECT_NE(hash, 0U);
  EXPECT_EQ(hash, HashAiDenoiseRun(version, kTestXyzToCamera));
  EXPECT_NE(hash, HashAiDenoiseRun(std::string(64, 'b'), kTestXyzToCamera));
  EXPECT_NE(hash, HashAiDenoiseRun(version, other_camera));
}

TEST(AiDenoiseModelSessionTest, NonBayerInputKeepsItsEntryWithoutARun) {
  PreparedSourceCache          cache([](std::span<const std::byte>, DecodeRes) {
    HostImagePlane rgba;
    rgba.extent       = Extent2D{16, 16};
    rgba.stride_bytes = 16 * 16;
    rgba.format       = HostPixelFormat::F32Rgba;
    auto storage      = std::shared_ptr<std::byte>(new std::byte[rgba.ByteCount()],
                                                            [](std::byte* p) { delete[] p; });
    std::fill_n(reinterpret_cast<float*>(storage.get()), rgba.ByteCount() / 4, 0.5f);
    rgba.bytes = std::const_pointer_cast<const std::byte>(storage);
    return RawInputLoader::FromDirectRgb(std::move(rgba), RawSensorGeometry{});
  });
  const std::vector<std::byte> encoded(64, std::byte{3});
  int                          runs = 0;
  const auto lease = cache.AcquireDenoised(EncodedBytes(encoded), std::string(64, 'a'),
                                           kTestXyzToCamera, [&](const PreparedRawInput& input) {
                                             ++runs;
                                             return input;
                                           });
  EXPECT_EQ(runs, 0);
  EXPECT_EQ(lease.Get().input_kind, RawInputKind::DebayeredRgb);
}

}  // namespace
}  // namespace alcedo
