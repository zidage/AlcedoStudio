//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/export_recipe.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace alcedo {

TEST(ExportResolutionTests, OriginalPixelsKeepDimensionsAndApplyOptionalDpiTag) {
  ExportResizeSpec spec;
  spec.dpi_         = 240.0;

  const auto result = ResolveExportResolution(spec, {.width_ = 6000, .height_ = 4000});

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.pixels_.width_, 6000);
  EXPECT_EQ(result.pixels_.height_, 4000);
  EXPECT_DOUBLE_EQ(result.dpi_, 240.0);
}

TEST(ExportResolutionTests, LongEdgePixelsKeepsAspectRatioAndDoesNotUpscaleByDefault) {
  ExportResizeSpec spec;
  spec.mode_             = ExportResizeMode::LONG_EDGE_PIXELS;
  spec.long_edge_pixels_ = 3000;

  const auto downscaled  = ResolveExportResolution(spec, {.width_ = 6000, .height_ = 4000});
  const auto unchanged   = ResolveExportResolution(spec, {.width_ = 1200, .height_ = 800});

  ASSERT_TRUE(downscaled.success_) << downscaled.message_;
  EXPECT_EQ(downscaled.pixels_.width_, 3000);
  EXPECT_EQ(downscaled.pixels_.height_, 2000);
  ASSERT_TRUE(unchanged.success_) << unchanged.message_;
  EXPECT_EQ(unchanged.pixels_.width_, 1200);
  EXPECT_EQ(unchanged.pixels_.height_, 800);
}

TEST(ExportResolutionTests, BoundingBoxPixelsUsesBothLimits) {
  ExportResizeSpec spec;
  spec.mode_          = ExportResizeMode::BOUNDING_BOX_PIXELS;
  spec.width_pixels_  = 2000;
  spec.height_pixels_ = 2000;

  const auto result   = ResolveExportResolution(spec, {.width_ = 6000, .height_ = 4000});

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.pixels_.width_, 2000);
  EXPECT_EQ(result.pixels_.height_, 1333);
}

TEST(ExportResolutionTests, PhysicalMillimetersAndDpiProducePixelDimensions) {
  ExportResizeSpec spec;
  spec.mode_           = ExportResizeMode::PHYSICAL_SIZE;
  spec.physical_width_ = 254.0;
  spec.physical_unit_  = ExportPhysicalUnit::MILLIMETERS;
  spec.dpi_            = 300.0;
  spec.allow_upscale_  = true;

  const auto result    = ResolveExportResolution(spec, {.width_ = 6000, .height_ = 4000});

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.pixels_.width_, 3000);
  EXPECT_EQ(result.pixels_.height_, 2000);
  EXPECT_DOUBLE_EQ(result.dpi_, 300.0);
}

TEST(ExportResolutionTests, PhysicalSizeRejectsMissingDpi) {
  ExportResizeSpec spec;
  spec.mode_           = ExportResizeMode::PHYSICAL_SIZE;
  spec.physical_width_ = 10.0;

  const auto result    = ResolveExportResolution(spec, {.width_ = 6000, .height_ = 4000});

  EXPECT_FALSE(result.success_);
  EXPECT_FALSE(result.message_.empty());
}

TEST(ExportResolutionTests, MaximumEdgeLimitCapsOriginalPixelsWithoutUpscaling) {
  ExportResizeSpec spec;
  spec.maximum_edge_pixels_ = 8192;

  const auto large          = ResolveExportResolution(spec, {.width_ = 12000, .height_ = 6000});
  const auto small          = ResolveExportResolution(spec, {.width_ = 4096, .height_ = 2048});

  ASSERT_TRUE(large.success_) << large.message_;
  EXPECT_EQ(large.pixels_.width_, 8192);
  EXPECT_EQ(large.pixels_.height_, 4096);
  ASSERT_TRUE(small.success_) << small.message_;
  EXPECT_EQ(small.pixels_.width_, 4096);
  EXPECT_EQ(small.pixels_.height_, 2048);
}

TEST(ExportResolutionTests, MaximumEdgeLimitAppliesAfterPhysicalSizeAndPreservesPrintSize) {
  ExportResizeSpec spec;
  spec.mode_                = ExportResizeMode::PHYSICAL_SIZE;
  spec.physical_width_      = 40.0;
  spec.physical_unit_       = ExportPhysicalUnit::INCHES;
  spec.dpi_                 = 300.0;
  spec.allow_upscale_       = true;
  spec.maximum_edge_pixels_ = 8192;

  const auto result         = ResolveExportResolution(spec, {.width_ = 12000, .height_ = 6000});

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.pixels_.width_, 8192);
  EXPECT_EQ(result.pixels_.height_, 4096);
  EXPECT_NEAR(result.dpi_, 204.8, 0.001);
}

TEST(ExportResolutionTests, EveryModeRejectsInvalidSourceDimensions) {
  for (const auto mode : {ExportResizeMode::ORIGINAL_PIXELS, ExportResizeMode::LONG_EDGE_PIXELS,
                          ExportResizeMode::BOUNDING_BOX_PIXELS, ExportResizeMode::PHYSICAL_SIZE}) {
    ExportResizeSpec spec;
    spec.mode_             = mode;
    spec.long_edge_pixels_ = 100;
    spec.width_pixels_     = 100;
    spec.physical_width_   = 1.0;
    spec.dpi_              = 100.0;
    EXPECT_FALSE(ResolveExportResolution(spec, {.width_ = 0, .height_ = 10}).success_);
  }
}

TEST(ExportFileNameTests, FieldsComposeInDeclaredOrderAndEncoderControlsExtension) {
  ExportFileNameTemplate name_template;
  name_template.parts_ = {
      {.field_ = ExportFileNameField::CAPTURE_DATE, .format_ = L"yyyy MM dd"},
      {.field_ = ExportFileNameField::LITERAL, .literal_ = L"_"},
      {.field_ = ExportFileNameField::CAMERA_MODEL},
      {.field_ = ExportFileNameField::LITERAL, .literal_ = L"_"},
      {.field_ = ExportFileNameField::SEQUENCE, .number_width_ = 4},
  };
  ExportFileNameContext context;
  context.capture_date_time_ = L"2024-04-06 10:55:04";
  context.camera_model_      = L"X-T5";
  context.sequence_          = 7;

  const auto result          = ResolveExportFileName(name_template, context, ImageFormatType::TIFF);

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.file_name_.wstring(), L"2024 04 06_X-T5_0007.tif");
}

TEST(ExportFileNameTests, SourceExtensionCannotOverrideEncoderExtension) {
  ExportFileNameTemplate name_template;
  name_template.parts_ = {
      {.field_ = ExportFileNameField::SOURCE_STEM},
      {.field_ = ExportFileNameField::LITERAL, .literal_ = L".png"},
  };
  ExportFileNameContext context;
  context.source_stem_ = L"DSCF2074";

  const auto result    = ResolveExportFileName(name_template, context, ImageFormatType::JPEG);

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.file_name_.extension().wstring(), L".jpg");
  EXPECT_EQ(result.file_name_.stem().wstring(), L"DSCF2074.png");
}

TEST(ExportFileNameTests, InvalidWindowsCharactersAreReplacedAndReservedStemIsChanged) {
  ExportFileNameTemplate invalid_template;
  invalid_template.parts_ = {
      {.field_ = ExportFileNameField::LITERAL, .literal_ = L"trip:day/one?"}};
  const auto invalid = ResolveExportFileName(invalid_template, {}, ImageFormatType::JPEG);
  ASSERT_TRUE(invalid.success_) << invalid.message_;
  EXPECT_EQ(invalid.file_name_.wstring(), L"trip_day_one_.jpg");

  ExportFileNameTemplate reserved_template;
  reserved_template.parts_ = {{.field_ = ExportFileNameField::LITERAL, .literal_ = L"CON"}};
  const auto reserved      = ResolveExportFileName(reserved_template, {}, ImageFormatType::PNG);
  ASSERT_TRUE(reserved.success_) << reserved.message_;
  EXPECT_EQ(reserved.file_name_.wstring(), L"CON_.png");
}

TEST(ExportFileNameTests, EmptyFieldsUseConfiguredFallback) {
  ExportFileNameTemplate name_template;
  name_template.parts_         = {{.field_ = ExportFileNameField::CAMERA_MODEL}};
  name_template.fallback_stem_ = L"untitled";

  const auto result            = ResolveExportFileName(name_template, {}, ImageFormatType::PNG);

  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.file_name_.wstring(), L"untitled.png");
}

TEST(ExportFileNameTests, UnsupportedOutputFormatReturnsAnError) {
  const auto result = ResolveExportFileName({}, {}, ImageFormatType::DNG);

  EXPECT_FALSE(result.success_);
  EXPECT_FALSE(result.message_.empty());
}

TEST(ExportFileNameTests, DeprecatedWebpAndBmpExportFormatsReturnAnError) {
  ExportFileNameTemplate name_template;
  name_template.parts_ = {{.field_ = ExportFileNameField::LITERAL, .literal_ = L"out"}};

  const auto webp = ResolveExportFileName(name_template, {}, ImageFormatType::WEBP);
  EXPECT_FALSE(webp.success_);
  EXPECT_FALSE(webp.message_.empty());

  const auto bmp = ResolveExportFileName(name_template, {}, ImageFormatType::BMP);
  EXPECT_FALSE(bmp.success_);
  EXPECT_FALSE(bmp.message_.empty());
}

TEST(ExportFileNameTests, PatternParserBuildsOrderedFieldsFormatsAndLiteralBraces) {
  const auto parsed =
      ParseExportFileNamePattern(L"{{{date:yyyy MM dd}}}-{cameraModel}-{source}-{sequence:0000}");
  ASSERT_TRUE(parsed.success_) << parsed.message_;

  const auto resolved = ResolveExportFileName(parsed.name_template_,
                                              {.source_stem_       = L"DSCF2074",
                                               .capture_date_time_ = L"2026:08:10 12:34:56",
                                               .camera_model_      = L"X-T5",
                                               .sequence_          = 7},
                                              ImageFormatType::TIFF);

  ASSERT_TRUE(resolved.success_) << resolved.message_;
  EXPECT_EQ(resolved.file_name_.wstring(), L"{2026 08 10}-X-T5-DSCF2074-0007.tif");
}

TEST(ExportFileNameTests, PatternParserRejectsUnknownFieldsAndInvalidSequenceFormats) {
  EXPECT_FALSE(ParseExportFileNamePattern(L"{unknown}").success_);
  EXPECT_FALSE(ParseExportFileNamePattern(L"{sequence:0001}").success_);
  EXPECT_FALSE(ParseExportFileNamePattern(L"{source").success_);
  EXPECT_FALSE(ParseExportFileNamePattern(L"source}").success_);
}

TEST(ExportRecipeTests, LegacyOptionsPreserveLongEdgeAndReplaceBehavior) {
  ExportFormatOptions options;
  options.resize_enabled_  = true;
  options.max_length_side_ = 2048;

  const auto recipe        = ExportRecipe::FromLegacyOptions(options);

  EXPECT_EQ(recipe.resize_.mode_, ExportResizeMode::LONG_EDGE_PIXELS);
  EXPECT_EQ(recipe.resize_.long_edge_pixels_, 2048);
  EXPECT_EQ(recipe.collision_, ExportCollisionPolicy::REPLACE);
  EXPECT_FALSE(recipe.output_color_.has_value());
  EXPECT_FALSE(ExportRecipeHasResolvedOutputColor(recipe));
}

TEST(ExportRecipeTests, ExportRecipeContainsResolvedOutputColorBeforeScheduling) {
  ExportRecipe missing = ExportRecipe::FromLegacyOptions({});
  EXPECT_THROW(RequireResolvedExportOutputColor(missing), std::runtime_error);
  missing.output_color_ = ExportColorProfileConfig{};
  missing.output_color_->peak_luminance = 0.0f;
  EXPECT_THROW(RequireResolvedExportOutputColor(missing), std::runtime_error);
  ExportRecipe recipe = ExportRecipe::FromLegacyOptions({});
  recipe.output_color_ = ExportColorProfileConfig{
      ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2, 100.0f};
  EXPECT_NO_THROW(RequireResolvedExportOutputColor(recipe));
  EXPECT_TRUE(ExportRecipeHasResolvedOutputColor(recipe));
}

}  // namespace alcedo
