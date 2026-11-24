//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Tests of the owner of the AI Denoise model session and of the denoised entry of the prepared
// source cache (integration plan, DN6), with a test platform.

#include "edit/input/ai_denoise_model_session.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ai_denoise_test_support.hpp"
#include "edit/input/ai_denoise_input.hpp"
#include "edit/input/prepared_source_cache.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

using ai_denoise_test::BayerPattern;
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
                                       std::string(64, 'a'));
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
  const auto            denoised = session->Denoise(input, version);

  EXPECT_EQ(denoised.input_kind, RawInputKind::DenoisedRgb);
  EXPECT_EQ(denoised.host_extent.width, 599U);
  EXPECT_EQ(denoised.host_extent.height, 203U);
  EXPECT_EQ(denoised.source_key.denoise_model_hash, HashAiDenoiseModelVersion(version));
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
  const double gain     = AiDenoiseGainFactor(AiDenoiseGainWeights(input.color_context.cam_xyz_),
                                              mosaic.input_means, sums.Means());
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
  const auto denoised = session->Denoise(input, version);
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
  (void)session->Denoise(MakeBayerInput(64, 48, BayerPattern("RGGB")), version);
  EXPECT_EQ(session->Stats().last_input_gain, 1.0);
}

TEST(AiDenoiseModelSessionTest, ModelSessionCreatesThePlatformSessionOnceAndAgainAfterRelease) {
  const TestModelFolder folder("session_lifetime");
  auto                  record  = std::make_shared<TestPlatformRecord>();
  auto                  session = MakeTestModelSession(record, folder);
  const auto            version = session->InstalledModelVersion();
  const auto            input   = MakeBayerInput(64, 48, BayerPattern("RGGB"));
  EXPECT_FALSE(session->IsSessionLoaded());

  (void)session->Denoise(input, version);
  (void)session->Denoise(input, version);
  EXPECT_EQ(record->sessions_created.load(), 1);
  EXPECT_EQ(record->sessions_alive.load(), 1);
  EXPECT_TRUE(session->IsSessionLoaded());
  EXPECT_EQ(session->Stats().denoise_runs, 2U);

  session->Release();
  EXPECT_FALSE(session->IsSessionLoaded());
  EXPECT_EQ(record->sessions_alive.load(), 0);

  (void)session->Denoise(input, version);
  EXPECT_EQ(record->sessions_created.load(), 2);

  // Another installed model version loads the graph again.
  (void)session->Denoise(input, std::string(64, 'e'));
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
    threads.emplace_back([&] { (void)session->Denoise(input, version); });
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

  EXPECT_EQ(ErrorKindOf([&] { (void)session->Denoise(input, version); }),
            AiDenoiseErrorKind::DeviceTooSlow);
  EXPECT_EQ(record->sessions_created.load(), 1);
  EXPECT_EQ(record->tile_runs.load(), 1);
  EXPECT_FALSE(session->IsSessionLoaded());

  session->Release();
  // A later run on that device stops at once: no session, no tile.
  EXPECT_EQ(ErrorKindOf([&] { (void)session->Denoise(input, version); }),
            AiDenoiseErrorKind::DeviceTooSlow);
  EXPECT_EQ(record->sessions_created.load(), 1);
  EXPECT_EQ(record->tile_runs.load(), 1);

  // Another device runs its tiles.
  record->device_key = "test device B";
  EXPECT_EQ(ErrorKindOf([&] { (void)session->Denoise(input, version); }),
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
    (void)session->Denoise(input, version);
    ADD_FAILURE() << "the run error was not reported";
  } catch (const AiDenoiseError& error) {
    EXPECT_EQ(error.Kind(), AiDenoiseErrorKind::RunFailed);
    EXPECT_NE(std::string(error.what()).find("887A0005"), std::string::npos) << error.what();
  }
  EXPECT_FALSE(session->IsSessionLoaded());
  EXPECT_EQ(record->sessions_alive.load(), 0);
  // The next run creates the session again.
  (void)session->Denoise(input, version);
  EXPECT_EQ(record->sessions_created.load(), 2);
}

TEST(AiDenoiseModelSessionTest, PlatformWithoutForwardCodeReportsRunFailed) {
  const TestModelFolder folder("no_platform");
  DenoiseModelSession   session(nullptr);
  session.SetModelFolderProvider([path = folder.Folder()] { return path; });
  const auto version = session.InstalledModelVersion();
  EXPECT_EQ(ErrorKindOf([&] {
              (void)session.Denoise(MakeBayerInput(32, 32, BayerPattern("RGGB")), version);
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
    return RawInputLoader::FromDenoisedBayer(cfa, std::move(rgba), std::string(64, 'a'));
  };
  const std::string version_a(64, 'a');
  const std::string version_b(64, 'b');

  {
    const auto lease = cache.AcquireDenoised(EncodedBytes(encoded), version_a, denoise);
    EXPECT_EQ(lease.Get().input_kind, RawInputKind::DenoisedRgb);
  }
  {
    const auto lease = cache.AcquireDenoised(EncodedBytes(encoded), version_a, denoise);
    EXPECT_EQ(lease.Get().input_kind, RawInputKind::DenoisedRgb);
  }
  EXPECT_EQ(runs, 1);
  EXPECT_EQ(unpacks, 1);
  EXPECT_EQ(cache.GetStats().denoise_runs, 1U);

  // Another installed model version is another entry; the CFA entry is still cached.
  (void)cache.AcquireDenoised(EncodedBytes(encoded), version_b, denoise);
  EXPECT_EQ(runs, 2);
  EXPECT_EQ(unpacks, 1);

  // A budget of one denoised entry: the unleased CFA entry is evicted before the denoised
  // entries, which keep the most recent one.
  const std::size_t denoised_bytes = std::size_t{64} * 48 * 16;
  cache.SetHostByteBudget(denoised_bytes);
  EXPECT_EQ(cache.EntryCount(), 1U);
  (void)cache.AcquireDenoised(EncodedBytes(encoded), version_b, denoise);
  EXPECT_EQ(runs, 2);
  // The switch off: the CFA entry is unpacked again.
  (void)cache.AcquireEncoded(EncodedBytes(encoded), DecodeRes::FULL);
  EXPECT_EQ(unpacks, 2);
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
                                           [&](const PreparedRawInput& input) {
                                             ++runs;
                                             return input;
                                           });
  EXPECT_EQ(runs, 0);
  EXPECT_EQ(lease.Get().input_kind, RawInputKind::DebayeredRgb);
}

}  // namespace
}  // namespace alcedo
