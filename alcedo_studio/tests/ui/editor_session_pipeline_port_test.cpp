//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

#include <gtest/gtest.h>

#include <string>

#include "../support/editor_lease_test_support.hpp"
#include "app/pipeline_service.hpp"

namespace alcedo::ui {
namespace {

TEST(EditorSessionPipelinePortTest, HeldLeaseExposesPreviewUntilRelease) {
  int                       acquire_count = 0;

  EditorSessionPipelinePort port;
  port.SetServices(EditorSessionPipelineMappers{{}, [&](sl_element_id_t element_id) {
                                                  ++acquire_count;
                                                  EXPECT_EQ(element_id, 42u);
                                                  return alcedo::test::MakeInMemoryEditorLease(
                                                      element_id);
                                                }});

  EXPECT_EQ(port.CurrentPreview(42), nullptr);

  std::string error;
  const auto  lease = port.AcquireLease(42, &error);
  ASSERT_TRUE(lease.has_value()) << error;
  ASSERT_NE(lease->graph_, nullptr);
  ASSERT_NE(lease->root_, nullptr);
  ASSERT_NE(lease->document_, nullptr);
  EXPECT_EQ(acquire_count, 1);
  EXPECT_EQ(port.CurrentPreview(42), lease->document_->CurrentPreview());
  EXPECT_NE(port.CurrentPreview(42), nullptr);

  EXPECT_FALSE(port.AcquireLease(42, &error).has_value());
  EXPECT_EQ(error, "The editor already holds image 42");
  EXPECT_EQ(acquire_count, 1);

  port.ReleaseLease(42);
  EXPECT_EQ(port.CurrentPreview(42), nullptr);
}

TEST(EditorSessionPipelinePortTest, AcquireLeaseReportsUnavailableService) {
  EditorSessionPipelinePort port;
  port.SetServices({});

  std::string error;
  EXPECT_FALSE(port.AcquireLease(42, &error).has_value());
  EXPECT_EQ(error, "Pipeline service is unavailable");
  EXPECT_EQ(port.CurrentPreview(42), nullptr);
}

}  // namespace
}  // namespace alcedo::ui
