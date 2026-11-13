//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The Develop raster `input` object, its edits, and the raster default document
// (raster_image_input_plan.md, sections 6.3, 6.5 and 7.3).

#include "edit/graph/develop_raster_input.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

auto DisplayP3Srgb() -> RasterColorDescription {
  RasterColorDescription description;
  description.primaries_xy_        = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
  description.origin_              = RasterColorOrigin::IccMatrixShaper;
  description.profile_description_ = "Display P3";
  description.icc_sha256_          = std::string(64, 'b');
  return description;
}

auto Rec2020Pq() -> RasterColorDescription {
  RasterColorDescription description;
  description.primaries_xy_ = color::GamutPrimariesXy(color::ColorGamutId::Rec2020);
  for (auto& transfer : description.transfer_) {
    transfer.kind_ = RasterTransferKind::St2084;
  }
  description.peak_luminance_nits_ = 4000.0f;
  description.origin_              = RasterColorOrigin::PngCicp;
  return description;
}

auto ExrLinear() -> RasterColorDescription {
  RasterColorDescription description;
  description.referral_     = RasterReferral::SceneLinear;
  description.primaries_xy_ = color::GamutPrimariesXy(color::ColorGamutId::Ap0);
  for (auto& transfer : description.transfer_) {
    transfer.kind_ = RasterTransferKind::Linear;
  }
  description.origin_ = RasterColorOrigin::ExrAcesContainer;
  return description;
}

TEST(DevelopRasterInputTest, RasterInputJsonRoundTripsThroughDevelopModel) {
  DevelopParamsModel model;
  auto               payload = model.Params();
  payload.input              = DevelopRasterInput{DisplayP3Srgb(), "adobe_rgb"};
  model.ReplaceParams(payload);
  const auto json = model.ToJson();
  ASSERT_TRUE(json.contains("input"));
  EXPECT_EQ(json.at("input").at("kind"), "raster");
  EXPECT_EQ(json.at("input").at("profile_override"), "adobe_rgb");
  EXPECT_EQ(json.at("input").at("source_color").at("origin"), "icc_matrix_shaper");

  DevelopParamsModel loaded;
  loaded.LoadJson(json);
  ASSERT_TRUE(loaded.RasterInput().has_value());
  EXPECT_EQ(*loaded.RasterInput(), (DevelopRasterInput{DisplayP3Srgb(), "adobe_rgb"}));
  EXPECT_EQ(loaded.ToJson().dump(), json.dump());
}

TEST(DevelopRasterInputTest, RawDevelopJsonHasNoInputKeyAndLoadsAsRaw) {
  DevelopParamsModel raw;
  EXPECT_FALSE(raw.ToJson().contains("input"));
  DevelopParamsModel loaded;
  loaded.LoadJson(raw.ToJson());
  EXPECT_FALSE(loaded.RasterInput().has_value());
}

TEST(DevelopRasterInputTest, MalformedInputObjectIsALoadErrorNotARawDocument) {
  DevelopParamsModel model;
  auto               json = model.ToJson();
  json["input"]           = {{"kind", "raster"}, {"profile_override", "auto"}};  // no source_color
  EXPECT_THROW(model.LoadJson(json), std::invalid_argument);
  json["input"] = DevelopRasterInputToJson(DevelopRasterInput{DisplayP3Srgb(), "auto"});
  json["input"]["profile_override"] = "log_c";
  EXPECT_THROW(model.LoadJson(json), std::invalid_argument);
}

TEST(DevelopRasterInputTest, RasterDocumentRejectsColorTemperatureWrite) {
  auto  document = CreateDefaultRasterPipelineDocument(DisplayP3Srgb());
  auto* develop  = document.Develop();
  ASSERT_NE(develop, nullptr);
  const auto                    before = develop->Params().ToJson();
  DevelopColorTemperatureUpdate update;
  update.wb_mode    = "custom";
  update.custom_cct = 4300.0f;
  EXPECT_THROW(develop->Params().ApplyColorTemperatureUpdate(update), std::logic_error);
  DevelopRawDecodeUpdate raw_update;
  raw_update.highlights_reconstruct = false;
  EXPECT_THROW(develop->Params().ApplyRawDecodeUpdate(raw_update), std::logic_error);
  EXPECT_EQ(develop->Params().ToJson(), before);
}

TEST(DevelopRasterInputTest, InputProfileUpdateMarksOnlyInputDirty) {
  auto       document     = CreateDefaultRasterPipelineDocument(DisplayP3Srgb());
  auto&      params       = document.Develop()->Params();
  const auto input_before = params.FieldsRevision(DirtyFieldMask{DevelopDirty::Input});
  const auto wb_before    = params.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});
  const auto lens_before  = params.FieldsRevision(DirtyFieldMask{DevelopDirty::Lens});
  params.ApplyInputProfileUpdate(DevelopInputProfileUpdate{"linear_rec709"});
  EXPECT_GT(params.FieldsRevision(DirtyFieldMask{DevelopDirty::Input}), input_before);
  EXPECT_EQ(params.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance}), wb_before);
  EXPECT_EQ(params.FieldsRevision(DirtyFieldMask{DevelopDirty::Lens}), lens_before);
  EXPECT_EQ(params.RasterInput()->profile_override_, "linear_rec709");

  const auto same = params.FieldsRevision(DirtyFieldMask{DevelopDirty::Input});
  params.ApplyInputProfileUpdate(DevelopInputProfileUpdate{"linear_rec709"});
  EXPECT_EQ(params.FieldsRevision(DirtyFieldMask{DevelopDirty::Input}), same);

  EXPECT_THROW(params.ApplyInputProfileUpdate(DevelopInputProfileUpdate{"rec601"}),
               std::invalid_argument);
  DevelopParamsModel raw;
  EXPECT_THROW(raw.ApplyInputProfileUpdate(DevelopInputProfileUpdate{"srgb"}), std::logic_error);
}

TEST(DevelopRasterInputTest, EffectiveDescriptionFollowsTheOverride) {
  DevelopRasterInput input{DisplayP3Srgb(), "auto"};
  EXPECT_EQ(ResolveEffectiveRasterDescription(input), DisplayP3Srgb());

  input.profile_override_ = "adobe_rgb";
  const auto adobe        = ResolveEffectiveRasterDescription(input);
  EXPECT_EQ(adobe.primaries_xy_, color::GamutPrimariesXy(color::ColorGamutId::AdobeRgb));
  EXPECT_EQ(adobe.transfer_[0].kind_, RasterTransferKind::Gamma);
  EXPECT_FLOAT_EQ(adobe.transfer_[0].gamma_, CE_ADOBE_RGB_GAMMA);
  EXPECT_EQ(adobe.referral_, RasterReferral::DisplayReferred);

  input.profile_override_ = "linear_rec709";
  const auto linear       = ResolveEffectiveRasterDescription(input);
  EXPECT_EQ(linear.referral_, RasterReferral::SceneLinear);
  EXPECT_EQ(linear.transfer_[2].kind_, RasterTransferKind::Linear);

  input.profile_override_ = "prophoto";
  EXPECT_EQ(ResolveEffectiveRasterDescription(input).primaries_xy_,
            color::GamutPrimariesXy(color::ColorGamutId::ProPhoto));
  for (const auto name : kRasterInputProfileOverrides) {
    input.profile_override_ = std::string(name);
    EXPECT_NO_THROW((void)ResolveEffectiveRasterDescription(input)) << name;
  }
}

TEST(DevelopRasterInputTest, DefaultRasterDocumentHasNeutralGradeAndMatchedAcesDrt) {
  const auto document = CreateDefaultRasterPipelineDocument(DisplayP3Srgb());
  ASSERT_TRUE(document.Develop()->Params().RasterInput().has_value());
  EXPECT_FALSE(document.Develop()->Params().LensEnabled());
  const auto* grade = document.PrimaryGrade();
  ASSERT_NE(grade, nullptr);
  const auto* exposure = grade->FindAdjustmentByType(type_ids::Exposure());
  ASSERT_NE(exposure, nullptr);
  EXPECT_FLOAT_EQ(exposure->ToJson().at("exposure_ev").get<float>(), 0.0f);
  const auto* contrast = grade->FindAdjustmentByType(type_ids::Contrast());
  ASSERT_NE(contrast, nullptr);
  EXPECT_FLOAT_EQ(contrast->ToJson().at("contrast").get<float>(), 0.0f);
  const auto* saturation = grade->FindAdjustmentByType(type_ids::Saturation());
  ASSERT_NE(saturation, nullptr);
  EXPECT_FLOAT_EQ(saturation->ToJson().at("saturation").get<float>(), 1.0f);

  const auto drt = document.Drt()->Params().Params();
  EXPECT_EQ(drt.method, DrtMethod::Aces20);
  EXPECT_EQ(drt.limiting_space, DrtColorSpace::P3D65);
  EXPECT_EQ(drt.encoding_space, DrtColorSpace::P3D65);
  EXPECT_EQ(drt.encoding_eotf, DrtEotf::SrgbPiecewise);
  EXPECT_FLOAT_EQ(drt.peak_luminance, 100.0f);

  const auto hdr = CreateDefaultRasterPipelineDocument(Rec2020Pq()).Drt()->Params().Params();
  EXPECT_EQ(hdr.limiting_space, DrtColorSpace::Rec2020);
  EXPECT_EQ(hdr.encoding_eotf, DrtEotf::St2084);
  EXPECT_FLOAT_EQ(hdr.peak_luminance, 4000.0f);

  const auto exr = CreateDefaultRasterPipelineDocument(ExrLinear()).Drt()->Params().Params();
  EXPECT_EQ(exr.limiting_space, DrtColorSpace::Rec709);
  EXPECT_EQ(exr.encoding_eotf, DrtEotf::SrgbPiecewise);
}

TEST(DevelopRasterInputTest, SrgbPiecewiseDrtEotfSerializesAsItsOwnString) {
  auto       document = CreateDefaultRasterPipelineDocument(DisplayP3Srgb());
  const auto json     = document.Drt()->Params().ToJson();
  EXPECT_EQ(json.at("encoding_eotf"), "srgb_piecewise");
  DrtParamsModel loaded;
  loaded.LoadJson(json);
  EXPECT_EQ(loaded.Params().encoding_eotf, DrtEotf::SrgbPiecewise);
}

}  // namespace
}  // namespace alcedo
