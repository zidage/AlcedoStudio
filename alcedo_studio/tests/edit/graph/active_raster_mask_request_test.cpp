//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../graph/grade_owned_mask_support.hpp"
#include "edit/graph/active_raster_mask_validation.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/pipeline/pipeline_apply_request.hpp"

namespace alcedo {
namespace {

auto MakePixels(std::uint8_t fill, std::uint32_t width = 4, std::uint32_t height = 3)
    -> std::shared_ptr<const std::vector<std::uint8_t>> {
  return std::make_shared<const std::vector<std::uint8_t>>(
      static_cast<std::size_t>(width) * height, fill);
}

auto MakeBrushInput(PipelineDocument& document, std::uint8_t fill = 17) -> ActiveRasterMaskInput {
  MaskAssetDescriptor descriptor;
  descriptor.extent = {4, 3};
  grade_mask_test::AddBrushMask(document, MaskId{"mask.brush"}, MaskAssetKey{"asset.settled"},
                                descriptor);
  ActiveRasterMaskInput input;
  input.owner_node_id      = document.PrimaryGrade()->Id();
  input.mask_id            = MaskId{"mask.brush"};
  input.session_generation = 1;
  input.content_revision   = 1;
  input.descriptor         = descriptor;
  input.pixels             = MakePixels(fill);
  input.dirty_rectangle    = {0, 0, 4, 3};
  return input;
}

TEST(GpuDagModelGraph, ActiveRasterInputsTravelAsImmutableSharedPixels) {
  PipelineApplyRequest request;
  const auto           snapshot =
      std::make_shared<const std::vector<std::uint8_t>>(12, std::uint8_t{9});
  ActiveRasterMaskInput input;
  input.owner_node_id     = NodeId{"grade.primary"};
  input.mask_id           = MaskId{"mask.brush"};
  input.pixels            = snapshot;
  input.descriptor.extent = {4, 3};
  input.dirty_rectangle   = {0, 0, 4, 3};
  request.active_raster_masks.push_back(input);
  EXPECT_EQ(request.active_raster_masks.front().pixels, snapshot);
  EXPECT_EQ(request.active_raster_masks.front().pixels->front(), 9U);
}

TEST(GpuDagModelGraph, InvalidActiveRasterTargetIsRejectedBeforeNativeUpload) {
  auto document = CreateDefaultPipelineDocument();
  auto input    = MakeBrushInput(document);
  input.mask_id = MaskId{"mask.missing"};
  EXPECT_THROW(ValidateActiveRasterMaskBindings(document, std::span{&input, 1}), std::runtime_error);

  auto radial_doc = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(radial_doc, MaskId{"mask.radial"});
  ActiveRasterMaskInput radial;
  radial.owner_node_id     = radial_doc.PrimaryGrade()->Id();
  radial.mask_id           = MaskId{"mask.radial"};
  radial.descriptor.extent = {4, 3};
  radial.pixels            = MakePixels(1);
  radial.dirty_rectangle   = {0, 0, 4, 3};
  EXPECT_THROW(ValidateActiveRasterMaskBindings(radial_doc, std::span{&radial, 1}),
               std::runtime_error);
}

TEST(GpuDagModelGraph, DuplicateActiveRasterIdentityIsRejected) {
  auto document = CreateDefaultPipelineDocument();
  auto first    = MakeBrushInput(document);
  auto second   = first;
  std::vector<ActiveRasterMaskInput> inputs{first, second};
  EXPECT_THROW(ValidateActiveRasterMaskFields(inputs), std::runtime_error);
}

TEST(GpuDagModelGraph, EmptyDirtyRectangleIsRejectedForActiveRaster) {
  auto document = CreateDefaultPipelineDocument();
  auto input    = MakeBrushInput(document);
  input.dirty_rectangle = {};
  EXPECT_THROW(ValidateActiveRasterMaskFields(std::span{&input, 1}), std::runtime_error);
  input.dirty_rectangle = {8, 8, 1, 1};
  EXPECT_THROW(ValidateActiveRasterMaskFields(std::span{&input, 1}), std::runtime_error);
}

TEST(GpuDagModelGraph, ActiveRasterDescriptorMismatchIsRejected) {
  auto document = CreateDefaultPipelineDocument();
  auto input    = MakeBrushInput(document);
  input.descriptor.extent = {8, 3};
  input.pixels            = MakePixels(1, 8, 3);
  input.dirty_rectangle   = {0, 0, 8, 3};
  EXPECT_THROW(ValidateActiveRasterMaskBindings(document, std::span{&input, 1}), std::runtime_error);
}

}  // namespace
}  // namespace alcedo
