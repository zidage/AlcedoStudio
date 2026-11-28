//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The AI Denoise input of the Develop passes without a GPU: the loader factory, the pass list,
// the content keys, and the input check (ai_denoise_integration_plan.md, DN5).

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <stdexcept>
#include <string>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/develop_ai_denoise.hpp"
#include "edit/runtime/develop_demosaic.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/result_content_key.hpp"

namespace alcedo {
namespace {

const std::string kVersionA(64, 'a');
const std::string kVersionB(64, 'b');
const std::array<double, 9> kCamera = {0.7, 0.2, 0.1, 0.3, 0.9, -0.1, 0.05, -0.1, 0.8};

/// 64 x 48 RGGB mosaic with a 2 px margin at the left and the top and a DNG default crop of
/// 56 x 40, so the decode area is not the whole plane. Orientation 6.
auto              MakeCfaInput(DecodeRes decode_res = DecodeRes::FULL) -> PreparedRawInput {
  auto sensor             = gpu_dag_test::FullSensor(64, 48);
  sensor.left_margin      = 2;
  sensor.top_margin       = 2;
  sensor.width            = 62;
  sensor.height           = 46;
  sensor.default_crop[0]  = 4;
  sensor.default_crop[1]  = 4;
  sensor.default_crop[2]  = 56;
  sensor.default_crop[3]  = 40;
  sensor.orientation_flip = 6;
  return RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 48, gpu_dag_test::MakeRggbPattern()),
      gpu_dag_test::MakeRggbPattern(), gpu_dag_test::NearBlackLinearization(), sensor, decode_res);
}

auto MakeDenoised(const PreparedRawInput& cfa, const std::string& version = kVersionA,
                  const std::array<double, 9>& camera = kCamera) -> PreparedRawInput {
  return RawInputLoader::FromDenoisedBayer(cfa, gpu_dag_test::MakeF32RgbaPlane(56, 40), version,
                                           camera);
}

TEST(DevelopAiDenoiseInputTest, DenoisedInputTakesTheCfaMetadataAndTheDecodeArea) {
  const auto cfa      = MakeCfaInput();
  const auto denoised = MakeDenoised(cfa);

  EXPECT_EQ(denoised.input_kind, RawInputKind::DenoisedRgb);
  EXPECT_EQ(denoised.CompileSource().kind, DevelopInputKind::DenoisedRgb);
  EXPECT_EQ(denoised.host_extent, (Extent2D{56, 40}));
  EXPECT_EQ(denoised.downsample_passes, 0);
  EXPECT_EQ(denoised.demosaic_output_crop, (RectI{0, 0, 56, 40}));
  // Orientation 6 turns the decode area.
  EXPECT_EQ(denoised.develop_output_extent, (Extent2D{40, 56}));
  EXPECT_EQ(denoised.full_reference_extent, (Extent2D{40, 56}));
  EXPECT_EQ(denoised.sensor.orientation_flip, 6);
  for (int c = 0; c < 4; ++c) {
    EXPECT_EQ(denoised.linearization.black_level[c], cfa.linearization.black_level[c]);
    EXPECT_EQ(denoised.linearization.white_level[c], cfa.linearization.white_level[c]);
    EXPECT_EQ(denoised.linearization.cam_mul[c], cfa.linearization.cam_mul[c]);
  }

  // Black 0, the relative as-shot white balance (2, 1, 1.5), and the clamp to [0, wb].
  ASSERT_TRUE(denoised.rgb_linearization.has_value());
  const auto& rgb = *denoised.rgb_linearization;
  EXPECT_EQ(rgb.integer_codes, 0U);
  EXPECT_EQ(rgb.clamp_to_range, 1U);
  const float wb[3] = {2.0f, 1.0f, 1.5f};
  for (int c = 0; c < 3; ++c) {
    EXPECT_EQ(rgb.black[c], 0.0f);
    EXPECT_FLOAT_EQ(rgb.scale[c], wb[c]);
    EXPECT_FLOAT_EQ(rgb.range_max[c], wb[c]);
  }
  // With the highlight recovery off, the range also holds the clamp after the white balance.
  const auto off = DevelopRgbLinearization(denoised, false);
  for (int c = 0; c < 3; ++c) {
    EXPECT_FLOAT_EQ(off.scale[c], wb[c]);
    EXPECT_FLOAT_EQ(off.range_max[c], 1.0f);
  }
  const auto on = DevelopRgbLinearization(denoised, true);
  EXPECT_FLOAT_EQ(on.range_max[0], 2.0f);

  // Encoded identity of the RAW file, the denoised kind, and the model hash.
  EXPECT_EQ(denoised.source_key.encoded_content_hash, cfa.source_key.encoded_content_hash);
  EXPECT_EQ(denoised.source_key.encoded_byte_count, cfa.source_key.encoded_byte_count);
  EXPECT_EQ(denoised.source_key.input_kind, RawInputKind::DenoisedRgb);
  EXPECT_EQ(denoised.source_key.denoise_hash, HashAiDenoiseRun(kVersionA, kCamera));
  EXPECT_NE(denoised.source_key.denoise_hash, 0U);
  EXPECT_EQ(cfa.source_key.denoise_hash, 0U);
  EXPECT_FALSE(MakeDenoised(cfa, kVersionB).source_key == denoised.source_key);
  // Another camera matrix of the gain match is another key (#1055).
  auto other_camera = kCamera;
  other_camera[0] += 0.05;
  EXPECT_FALSE(MakeDenoised(cfa, kVersionA, other_camera).source_key == denoised.source_key);

  // A file whose white balance LibRaw applied gets no white balance gain.
  auto applied                           = cfa;
  applied.linearization.apply_as_shot_wb = 0;
  const auto unbalanced                  = MakeDenoised(applied);
  for (int c = 0; c < 3; ++c) {
    EXPECT_FLOAT_EQ(unbalanced.rgb_linearization->scale[c], 1.0f);
  }
}

TEST(DevelopAiDenoiseInputTest, DenoisedInputRejectsAReducedOrNonBayerInputAndAWrongPlane) {
  const auto cfa = MakeCfaInput();
  EXPECT_THROW((void)MakeDenoised(MakeCfaInput(DecodeRes::HALF)), std::runtime_error);

  const auto xtrans = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(60, 48, gpu_dag_test::MakeXTransPattern()),
      gpu_dag_test::MakeXTransPattern(), gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(60, 48), DecodeRes::FULL);
  EXPECT_THROW((void)RawInputLoader::FromDenoisedBayer(
                   xtrans, gpu_dag_test::MakeF32RgbaPlane(60, 48), kVersionA, kCamera),
               std::runtime_error);

  const auto direct = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(56, 40),
                                                    gpu_dag_test::FullSensor(56, 40));
  EXPECT_THROW((void)RawInputLoader::FromDenoisedBayer(
                   direct, gpu_dag_test::MakeF32RgbaPlane(56, 40), kVersionA, kCamera),
               std::runtime_error);

  // The whole mosaic is not the decode area.
  EXPECT_THROW((void)RawInputLoader::FromDenoisedBayer(cfa, gpu_dag_test::MakeF32RgbaPlane(64, 48),
                                                       kVersionA, kCamera),
               std::runtime_error);
  auto u16 = gpu_dag_test::MakeU16CfaPlane(56, 40, gpu_dag_test::MakeRggbPattern());
  EXPECT_THROW((void)RawInputLoader::FromDenoisedBayer(cfa, u16, kVersionA, kCamera),
               std::runtime_error);
  for (const auto& version : {std::string("abc"), std::string(64, 'A'), std::string()}) {
    EXPECT_THROW((void)RawInputLoader::FromDenoisedBayer(
                     cfa, gpu_dag_test::MakeF32RgbaPlane(56, 40), version, kCamera),
                 std::runtime_error);
  }
}

TEST(DevelopAiDenoiseInputTest, DenoisedInputCompilesTheRgbUploadWithHighlightRecovery) {
  const auto denoised = MakeDenoised(MakeCfaInput());
  auto       document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  document.Develop()->Params().ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersionA});
  const auto plan = GraphCompiler::Compile(document, denoised.CompileSource(), RenderRequest{});

  const GpuPassKind expected[] = {GpuPassKind::UploadRgb,         GpuPassKind::HighlightRecover,
                                  GpuPassKind::InverseCamMulPack, GpuPassKind::Lens,
                                  GpuPassKind::GeometryResample,  GpuPassKind::CameraToAp1};
  int               previous   = -1;
  for (const auto kind : expected) {
    const int index = plan.IndexOf(kind);
    EXPECT_GT(index, previous) << static_cast<int>(kind);
    previous = index;
  }
  for (const auto absent : {GpuPassKind::UploadRaw, GpuPassKind::Linearize, GpuPassKind::CfaClamp,
                            GpuPassKind::Demosaic, GpuPassKind::DisplayToAp1}) {
    EXPECT_FALSE(plan.Contains(absent)) << static_cast<int>(absent);
  }
  EXPECT_EQ(plan.source.kind, DevelopInputKind::DenoisedRgb);
  EXPECT_EQ(plan.source.develop_output_extent, (Extent2D{40, 56}));

  // The demosaic method resolves to Legacy, so no demosaic model runs.
  auto payload            = document.Develop()->Params().Params();
  payload.demosaic_method = "neural_engine";
  EXPECT_EQ(ResolveDevelopDemosaicMethod(payload, denoised.CompileSource()),
            RawDemosaicMethod::Legacy);
}

TEST(DevelopAiDenoiseInputTest, SensorKeyHoldsTheSwitchAndTheModelVersion) {
  const auto cfa      = MakeCfaInput();
  const auto denoised = MakeDenoised(cfa);
  auto       document = CreateDefaultPipelineDocument();
  gpu_dag_test::EnsureTestCameraProfile(document);
  const auto cfa_plan = GraphCompiler::Compile(document, cfa.CompileSource(), RenderRequest{});
  const auto off      = BuildFrameResultContentKeys(cfa_plan, cfa, document);

  auto&      develop  = document.Develop()->Params();
  develop.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersionA});
  const auto on_cfa = BuildFrameResultContentKeys(cfa_plan, cfa, document);
  EXPECT_NE(on_cfa.sensor_linear, off.sensor_linear);
  EXPECT_NE(on_cfa.develop_image, off.develop_image);

  // Another recorded version alone: the render uses the installed model.
  develop.ApplyAiDenoiseUpdate(DevelopAiDenoise{kVersionB});
  EXPECT_EQ(BuildFrameResultContentKeys(cfa_plan, cfa, document).sensor_linear,
            on_cfa.sensor_linear);

  // The installed model version is in the prepared source.
  const auto plan    = GraphCompiler::Compile(document, denoised.CompileSource(), RenderRequest{});
  const auto model_a = BuildFrameResultContentKeys(plan, denoised, document);
  const auto model_b = BuildFrameResultContentKeys(plan, MakeDenoised(cfa, kVersionB), document);
  EXPECT_NE(model_a.sensor_linear, model_b.sensor_linear);
  EXPECT_NE(model_a.sensor_linear, on_cfa.sensor_linear);

  develop.ApplyAiDenoiseUpdate(std::nullopt);
  const auto off_again = BuildFrameResultContentKeys(cfa_plan, cfa, document);
  EXPECT_EQ(off_again.sensor_linear, off.sensor_linear);
  EXPECT_EQ(off_again.develop_image, off.develop_image);
}

TEST(DevelopAiDenoiseInputTest, InputCheckRejectsAFullSizeInputThatIsNotDenoised) {
  DevelopPayload off;
  DevelopPayload on;
  on.ai_denoise     = DevelopAiDenoise{kVersionA};

  const auto source = [](DevelopInputKind kind, std::uint8_t passes) {
    DevelopCompileSource result;
    result.kind              = kind;
    result.downsample_passes = passes;
    return result;
  };
  // Switch on: only the denoised input renders at full size; a reduced input renders the CFA.
  EXPECT_NO_THROW(RequireDevelopInputForAiDenoise(on, source(DevelopInputKind::DenoisedRgb, 0)));
  EXPECT_NO_THROW(RequireDevelopInputForAiDenoise(on, source(DevelopInputKind::BayerCfa, 1)));
  EXPECT_NO_THROW(RequireDevelopInputForAiDenoise(on, source(DevelopInputKind::BayerCfa, 3)));
  try {
    RequireDevelopInputForAiDenoise(on, source(DevelopInputKind::BayerCfa, 0));
    ADD_FAILURE() << "a full-size Bayer CFA input was accepted";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "AI Denoise is on and the prepared input is not denoised");
  }
  // A photo that is not Bayer: the typed error of DN7, whose kind the editor shows.
  for (const auto kind : {DevelopInputKind::XTransCfa, DevelopInputKind::DirectRgb}) {
    try {
      RequireDevelopInputForAiDenoise(on, source(kind, 0));
      ADD_FAILURE() << "kind " << static_cast<int>(kind) << " was accepted";
    } catch (const AiDenoiseError& error) {
      EXPECT_EQ(error.Kind(), AiDenoiseErrorKind::SensorNotSupported);
      EXPECT_EQ(error.Text(), "AI Denoise supports Bayer sensors only");
    }
  }

  // Switch off: every input except the denoised one.
  for (const auto kind : {DevelopInputKind::BayerCfa, DevelopInputKind::XTransCfa,
                          DevelopInputKind::DirectRgb, DevelopInputKind::Raster}) {
    EXPECT_NO_THROW(RequireDevelopInputForAiDenoise(off, source(kind, 0)));
  }
  EXPECT_THROW(RequireDevelopInputForAiDenoise(off, source(DevelopInputKind::DenoisedRgb, 0)),
               std::runtime_error);
}

}  // namespace
}  // namespace alcedo
