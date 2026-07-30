//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

#include <gtest/gtest.h>

#include <memory>

#include "app/pipeline_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"

namespace alcedo::ui {
namespace {

auto MakeGuard(sl_element_id_t element_id) -> std::shared_ptr<alcedo::PipelineGuard> {
  auto guard       = std::make_shared<alcedo::PipelineGuard>();
  guard->id_       = element_id;
  guard->pipeline_ = std::make_shared<alcedo::CPUPipelineExecutor>();
  return guard;
}

TEST(EditorSessionPipelinePortTest, CachesLoadedGuardUntilRelease) {
  RegisterAllOperators();
  auto                      loaded     = MakeGuard(42);
  int                       load_count = 0;

  EditorSessionPipelinePort port;
  port.SetServices(EditorSessionPipelineServices{{}, [&](sl_element_id_t element_id) {
                                                   ++load_count;
                                                   EXPECT_EQ(element_id, 42u);
                                                   return loaded;
                                                 }});

  const auto handle = port.Acquire(42, nullptr);
  ASSERT_TRUE(handle.valid);
  EXPECT_EQ(port.CurrentGuard(42), nullptr);

  const auto first = port.EnsureLoaded(42, nullptr);
  ASSERT_EQ(first, loaded);
  EXPECT_EQ(port.EnsureLoaded(42, nullptr), loaded);
  EXPECT_EQ(load_count, 1);
  EXPECT_EQ(port.CurrentGuard(42), loaded);

  port.Release(handle);
  EXPECT_EQ(port.CurrentGuard(42), nullptr);
}

TEST(EditorSessionPipelinePortTest, ReportsUnavailableLoader) {
  EditorSessionPipelinePort port;
  port.SetServices({});

  std::string error;
  EXPECT_EQ(port.EnsureLoaded(42, &error), nullptr);
  EXPECT_EQ(error, "Pipeline service is unavailable");
}

}  // namespace
}  // namespace alcedo::ui
