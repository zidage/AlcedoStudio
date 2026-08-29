//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

namespace alcedo {

TEST(GpuDagModelGraph, RepeatedExposureWritesCollapseIntoOneDirtyPatch) {
  ExposureModel model;
  model.SetValue(0.1f);
  model.SetValue(0.2f);
  model.SetValue(0.3f);

  const auto patch = model.TakeDirtyPatch();
  ASSERT_TRUE(patch.has_value());
  const auto* payload = PayloadAs<ScalarFloatPayload>(patch->payload.get());
  ASSERT_NE(payload, nullptr);
  EXPECT_FLOAT_EQ(payload->value, 0.3f);
  EXPECT_FALSE(model.IsDirty());
  EXPECT_FALSE(model.TakeDirtyPatch().has_value());
}

TEST(GpuDagModelGraph, DirtyPatchTakenBeforeNewEditLeavesNewEditDirty) {
  ExposureModel model;
  model.SetValue(0.2f);
  ASSERT_TRUE(model.TakeDirtyPatch().has_value());
  EXPECT_FALSE(model.IsDirty());

  model.SetValue(0.5f);
  EXPECT_TRUE(model.IsDirty());
  const auto patch = model.TakeDirtyPatch();
  ASSERT_TRUE(patch.has_value());
  const auto* payload = PayloadAs<ScalarFloatPayload>(patch->payload.get());
  ASSERT_NE(payload, nullptr);
  EXPECT_FLOAT_EQ(payload->value, 0.5f);
}

TEST(GpuDagModelGraph, CancelledParameterTransferRestoresDirtyFields) {
  ExposureModel model;
  model.SetValue(1.25f);
  {
    auto pending = TakePendingParameterPatch(model);
    ASSERT_TRUE(pending.has_value());
    EXPECT_FALSE(model.IsDirty());
  }
  EXPECT_TRUE(model.IsDirty());
  const auto retry = model.TakeDirtyPatch();
  ASSERT_TRUE(retry.has_value());
  const auto* payload = PayloadAs<ScalarFloatPayload>(retry->payload.get());
  ASSERT_NE(payload, nullptr);
  EXPECT_FLOAT_EQ(payload->value, 1.25f);
}

TEST(GpuDagModelGraph, MakeFullDtoIgnoresDirtyState) {
  ExposureModel model;
  model.SetValue(0.75f);
  ASSERT_TRUE(model.TakeDirtyPatch().has_value());
  EXPECT_FALSE(model.IsDirty());
  const auto dto      = model.MakeFullDto();
  const auto* payload = PayloadAs<ScalarFloatPayload>(dto.payload.get());
  ASSERT_NE(payload, nullptr);
  EXPECT_FLOAT_EQ(payload->value, 0.75f);
}

TEST(GpuDagModelGraph, CommittedParameterTransferLeavesFieldsClean) {
  ExposureModel model;
  model.SetValue(0.4f);
  {
    auto pending = TakePendingParameterPatch(model);
    ASSERT_TRUE(pending.has_value());
    pending->Commit();
  }
  EXPECT_FALSE(model.IsDirty());
}

}  // namespace alcedo
